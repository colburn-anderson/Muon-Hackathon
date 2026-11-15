"""
roomlock API: one Lambda behind an API Gateway HTTP API (payload v2), plus the
EventBridge Scheduler tick.

Table: roomlock-db, single-table design
  PK room_id (S)   SK sk (S)
  sk = "STATE"                 live room status: presence, presence_seen_at,
                               res_active, session{res_id,user,kind,started_at,expires_at}
  sk = "RES#<res_id>"          one booking: start_ts, end_ts, user, checked_in, status
  sk = "SLOT#YYYY-MM-DDTHH:MMZ" one taken slot (15 min unless an admin changed it): res_id, kind
  room_id "_admin", sk "SETTINGS"  admin settings: seconds_per_minute, slot_minutes. Not a room:
                               every room scan filters on sk = "STATE".

Invariant the whole file relies on:
  every slot-sized window that someone holds has a SLOT# row, and every write that
  takes a window does a conditional put (attribute_not_exists). That is the
  only place double-booking is prevented, so it can't be bypassed.
  An active session owns every SLOT# row covering [now, expires_at).
  All live SLOT# rows share one slot length, which is why the admin can only change it
  while no current or future slot is taken.

Env vars:
  TABLE_NAME          default "roomlock-db"
  SECONDS_PER_MINUTE  default 60. Set to 6 for the demo clock (plan §8).
                      Scales the rule durations, NOT the 15-min slot size.
                      The admin page's setting (the SETTINGS row) overrides it.
  SLOT_MINUTES        default 15. Booking slot length; the SETTINGS row overrides it.
  DEVICE_KEYS         JSON {"1": "<key>", ...}. Put it in the Lambda config, not git.
  ALLOW_DEV_USER      "1" lets an x-user header stand in for Cognito (local testing only).
"""

import hmac
import json
import os
import time
import uuid
from datetime import datetime, timezone
from decimal import Decimal

import boto3
from boto3.dynamodb.conditions import Attr, Key
from botocore.exceptions import ClientError

TABLE_NAME = os.environ.get("TABLE_NAME", "roomlock-db")
SITE_URL = os.environ.get("SITE_URL", "https://main.d24j7edplzktpl.amplifyapp.com").rstrip("/")
DEFAULT_MINUTE = float(os.environ.get("SECONDS_PER_MINUTE", "60"))
SETTINGS_KEY = {"room_id": "_admin", "sk": "SETTINGS"}

DEFAULT_SLOT_MINUTES = int(os.environ.get("SLOT_MINUTES", "15"))
SLOT_CHOICES = (1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60)  # divide an hour; the key format has minute resolution
SLOT = DEFAULT_SLOT_MINUTES * 60  # booking slot length in seconds; set_slot() changes it
MAX_SLOTS = 96                  # 24 h at 15-min slots. DynamoDB transactions cap at 100 items.


def set_slot(minutes):
    global SLOT
    SLOT = int(minutes) * 60


def set_speed(seconds_per_minute):
    """Every rule duration comes from this one dial. 60 = real time; 3 = a 10-min rule lasts 30 s."""
    global MINUTE, TICK, VOID, SOFT_LEAD, NO_SHOW, WARN, EARLY_CHECKIN
    MINUTE = float(seconds_per_minute)
    TICK = int(5 * MINUTE)          # extension step
    VOID = int(10 * MINUTE)         # presence void -> end
    SOFT_LEAD = int(10 * MINUTE)    # no walk-up claim if a booking starts within this
    NO_SHOW = int(10 * MINUTE)      # release unchecked-in reservations after this
    WARN = int(5 * MINUTE)          # amber warning before a forced end
    EARLY_CHECKIN = int(5 * MINUTE)


set_speed(DEFAULT_MINUTE)

_table = None


def table():
    global _table
    if _table is None:
        _table = boto3.resource("dynamodb").Table(TABLE_NAME)
    return _table


class ApiError(Exception):
    def __init__(self, status, msg):
        super().__init__(msg)
        self.status, self.msg = status, msg


# --------------------------------------------------------------------- time/slots

def now():
    return int(time.time())


def slot_floor(ts):
    return ts - ts % SLOT


def slot_sk(ts):
    return "SLOT#" + datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%dT%H:%MZ")


def slot_ts(sk):
    dt = datetime.strptime(sk[5:], "%Y-%m-%dT%H:%MZ").replace(tzinfo=timezone.utc)
    return int(dt.timestamp())


def slots_covering(start, end):
    """Start times of every slot that overlaps [start, end)."""
    return list(range(slot_floor(start), end, SLOT))


# --------------------------------------------------------------------- dynamo helpers

def _av(d):
    # Plain Python values on purpose: table().meta.client belongs to the boto3 *resource*,
    # which converts Python values to DynamoDB's typed format itself. Serializing here too
    # would double-wrap every value ("3" -> {"M": {"S": ...}}) and DynamoDB rejects the key.
    return dict(d)


def _op(kind, cond=None, names=None, values=None, **body):
    if cond:
        body["ConditionExpression"] = cond
    if names:
        body["ExpressionAttributeNames"] = names
    if values:
        body["ExpressionAttributeValues"] = _av(values)
    body["TableName"] = TABLE_NAME
    return {kind: body}


def tx_put(item, cond="attribute_not_exists(sk)", **kw):
    return _op("Put", cond=cond, Item=_av(item), **kw)


def tx_update(key, expr, **kw):
    return _op("Update", Key=_av(key), UpdateExpression=expr, **kw)


def tx_delete(key, **kw):
    return _op("Delete", Key=_av(key), **kw)


def tx_check(key, cond, **kw):
    return _op("ConditionCheck", Key=_av(key), cond=cond, **kw)


def transact(ops, conflict_msg):
    try:
        table().meta.client.transact_write_items(TransactItems=ops)
    except ClientError as e:
        if e.response["Error"]["Code"] == "TransactionCanceledException":
            # one reason per op, in order: "None" means that op was fine
            reasons = [r.get("Code") + (f": {r['Message']}" if r.get("Message") else "")
                       for r in e.response.get("CancellationReasons", [])]
            print(json.dumps({"tx_cancelled": conflict_msg, "reasons": reasons}))
            raise ApiError(409, conflict_msg)
        raise


def slot_put(room_id, ts, res_id, kind):
    return tx_put({"room_id": room_id, "sk": slot_sk(ts), "res_id": res_id, "kind": kind})


def is_present(st):
    """presence is a Boolean; also reads the old number form (1/0) during the switch."""
    v = st.get("presence", False)
    return v is True or (not isinstance(v, bool) and v == 1)


def get_state(room_id):
    item = table().get_item(Key={"room_id": room_id, "sk": "STATE"}, ConsistentRead=True).get("Item")
    if not item:
        raise ApiError(404, f"room {room_id} not found")
    return item


def query_slots(room_id, start, end):
    """SLOT# rows with slot start in [start, end)."""
    if end <= start:
        return []
    resp = table().query(
        KeyConditionExpression=Key("room_id").eq(room_id)
        & Key("sk").between(slot_sk(slot_floor(start)), slot_sk(end - 1)),
        ConsistentRead=True,
    )
    return resp["Items"]


def query_reservations(room_id):
    resp = table().query(
        KeyConditionExpression=Key("room_id").eq(room_id) & Key("sk").begins_with("RES#"),
        ConsistentRead=True,
    )
    return resp["Items"]


# --------------------------------------------------------------------- session logic

def next_other_slot(room_id, t, own_res_id=None):
    """Start of the first held slot from the current slot onward that isn't ours."""
    for s in query_slots(room_id, t, t + 2 * 86400):
        if s["res_id"] != own_res_id:
            return slot_ts(s["sk"])
    return None


def end_session(room_id, st, reason):
    sess = st.get("session")
    if not sess:
        return
    rid, t = sess["res_id"], now()
    ops = [tx_update(
        {"room_id": room_id, "sk": "STATE"},
        "REMOVE #s SET res_active = :z",
        cond="#s.res_id = :rid",
        names={"#s": "session"},
        values={":z": 0, ":rid": rid},
    )]
    if sess["kind"] == "hard":
        ops.append(tx_update(
            {"room_id": room_id, "sk": f"RES#{rid}"},
            "SET #st = :done, ended_at = :t, end_reason = :r",
            names={"#st": "status"},
            values={":done": "ended", ":t": t, ":r": reason},
        ))
    # free the current slot and everything after it that this session holds
    for s in query_slots(room_id, t, t + (MAX_SLOTS + 1) * SLOT):
        if s["res_id"] == rid and len(ops) < 100:
            ops.append(tx_delete({"room_id": room_id, "sk": s["sk"]}))
    try:
        transact(ops, "session changed underneath us")
    except ApiError:
        pass  # someone else already ended it
    st.pop("session", None)
    st["res_active"] = 0


def try_extend(room_id, st):
    sess = st["session"]
    old, new = int(sess["expires_at"]), int(sess["expires_at"]) + TICK
    ops = [tx_update(
        {"room_id": room_id, "sk": "STATE"},
        "SET #s.expires_at = :new",
        cond="#s.res_id = :rid AND #s.expires_at = :old",
        names={"#s": "session"},
        values={":new": new, ":rid": sess["res_id"], ":old": old},
    )]
    held = {s["sk"]: s for s in query_slots(room_id, old, new)}
    for ts in slots_covering(old, new):
        row = held.get(slot_sk(ts))
        if row is None:
            ops.append(slot_put(room_id, ts, sess["res_id"], sess["kind"]))
        elif row["res_id"] != sess["res_id"]:
            return False  # another booking starts before the new expiry
    try:
        transact(ops, "extension conflict")
    except ApiError:
        return False
    sess["expires_at"] = new
    return True


def release_no_shows(room_id, t):
    for r in query_reservations(room_id):
        if r.get("status") == "booked" and int(r["start_ts"]) + NO_SHOW <= t:
            rid = r["sk"][4:]
            ops = [tx_update(
                {"room_id": room_id, "sk": r["sk"]},
                "SET #st = :ns",
                cond="#st = :b",
                names={"#st": "status"},
                values={":ns": "no_show", ":b": "booked"},
            )]
            for s in query_slots(room_id, int(r["start_ts"]), int(r["end_ts"])):
                if s["res_id"] == rid:
                    ops.append(tx_delete({"room_id": room_id, "sk": s["sk"]}))
            try:
                transact(ops, "no-show race")
            except ApiError:
                pass


def maintain(room_id, st=None):
    """Apply the §2 rules to one room. Idempotent, safe to call often.

    Runs on the scheduled tick and on every device poll, so expiry is never
    more than one poll late even with a shortened demo clock.
    """
    st = st or get_state(room_id)
    t = now()
    if "session" in st and not st["session"]:
        # an empty/null session (e.g. added by hand in the console) blocks claims: clear it
        table().update_item(Key={"room_id": room_id, "sk": "STATE"},
                            UpdateExpression="REMOVE #s", ExpressionAttributeNames={"#s": "session"})
        st.pop("session")
    release_no_shows(room_id, t)
    sess = st.get("session")
    if sess:
        seen = int(st.get("presence_seen_at", 0))
        exp = int(sess["expires_at"])
        if t - seen >= VOID:
            end_session(room_id, st, "presence_void")
        elif t >= exp:
            end_session(room_id, st, "expired")
        elif exp - t <= TICK:
            try_extend(room_id, st)
    return st


def room_view(room_id, st, user=None):
    t = now()
    sess = st.get("session")
    own = sess["res_id"] if sess else None
    nxt = next_other_slot(room_id, t, own)
    seen = int(st.get("presence_seen_at", 0))
    if sess:
        exp = int(sess["expires_at"])
        forced_soon = nxt is not None and nxt - t <= WARN
        void_soon = t - seen >= VOID - WARN
        state = "warn" if (forced_soon or void_soon) else "held"
    elif nxt is not None and nxt - t <= SOFT_LEAD:
        state = "reserved"
    else:
        state = "free"
    view = {
        "room_id": room_id,
        "state": state,
        "presence": is_present(st),
        "expires_at": int(sess["expires_at"]) if sess else None,
        "next_res_start": nxt,
        "slot_seconds": SLOT,
    }
    if sess and user:  # web routes are all behind the login check, so holder is safe here
        view["holder"] = sess["user"]
        view["yours"] = sess["user"] == user
        view["kind"] = sess["kind"]
    return view


# --------------------------------------------------------------------- handlers

def h_list_rooms(ev, user):
    items = table().scan(FilterExpression=Attr("sk").eq("STATE"))["Items"]
    rooms = sorted(items, key=lambda i: str(i["room_id"]))
    return {"rooms": [room_view(r["room_id"], r, user) for r in rooms], "slot_seconds": SLOT}


def h_get_room(ev, user):
    rid = ev["pathParameters"]["id"]
    return room_view(rid, maintain(rid), user)


def h_slots(ev, user):
    """Booked and free slots between ?from= and ?to= (epoch seconds)."""
    rid = ev["pathParameters"]["id"]
    get_state(rid)
    q = ev.get("queryStringParameters") or {}
    t = now()
    start = int(q.get("from", slot_floor(t)))
    end = int(q.get("to", start + 86400))
    if end - start > 7 * 86400:
        raise ApiError(400, "range too large (max 7 days)")
    taken = {s["sk"]: s for s in query_slots(rid, start, end)}
    out = []
    for ts in slots_covering(start, end):
        row = taken.get(slot_sk(ts))
        out.append({"start": ts, "end": ts + SLOT, "free": row is None,
                    "kind": row["kind"] if row else None})
    return {"room_id": rid, "slots": out}


NO_SESSION = "attribute_not_exists(#s)"  # maintain() removes an empty session left in the table


def h_claim(ev, user):
    rid = ev["pathParameters"]["id"]
    st = maintain(rid)
    t = now()
    if st.get("session"):
        raise ApiError(409, "room is already held")
    if is_present(st):
        raise ApiError(409, "someone is in the room without a session")
    if query_slots(rid, t, t + SOFT_LEAD):
        raise ApiError(409, "a reservation starts within 10 minutes")
    sid = "s_" + uuid.uuid4().hex[:10]
    exp = t + TICK
    sess = {"res_id": sid, "user": user, "kind": "soft", "started_at": t, "expires_at": exp}
    ops = [tx_update(
        {"room_id": rid, "sk": "STATE"},
        "SET #s = :sess, res_active = :one, presence_seen_at = :t",
        cond=NO_SESSION,  # presence is checked just above, in Python
        names={"#s": "session"},
        values={":sess": sess, ":one": 1, ":t": t},
    )]
    ops += [slot_put(rid, ts, sid, "soft") for ts in slots_covering(t, exp)]
    transact(ops, "room was taken a moment ago")
    st.update(session=sess, res_active=1, presence_seen_at=t)
    return room_view(rid, st, user)


def h_checkin(ev, user):
    rid = ev["pathParameters"]["id"]
    st = maintain(rid)
    t = now()
    mine = [r for r in query_reservations(rid)
            if r["user"] == user and r.get("status") in ("booked", "checked_in")
            and int(r["start_ts"]) - EARLY_CHECKIN <= t < int(r["end_ts"])]
    if not mine:
        raise ApiError(404, "no reservation of yours to check in to right now")
    r = mine[0]
    res_id = r["sk"][4:]
    sess = st.get("session")
    if sess and sess["res_id"] == res_id:
        return room_view(rid, st, user)  # already checked in
    if sess:
        raise ApiError(409, "room is still held by someone else")
    if r["status"] != "booked":
        raise ApiError(409, "reservation already used")
    start, end = int(r["start_ts"]), int(r["end_ts"])
    new = {"res_id": res_id, "user": user, "kind": "hard", "started_at": t, "expires_at": end}
    ops = [
        tx_update(
            {"room_id": rid, "sk": "STATE"},
            "SET #s = :sess, res_active = :one, presence_seen_at = :t",
            cond=NO_SESSION,
            names={"#s": "session"},
            values={":sess": new, ":one": 1, ":t": t},
        ),
        tx_update(
            {"room_id": rid, "sk": r["sk"]},
            "SET checked_in = :y, #st = :ci",
            cond="#st = :b",
            names={"#st": "status"},
            values={":y": True, ":ci": "checked_in", ":b": "booked"},
        ),
    ]
    # early check-in: also take the slots between now and the booked start
    ops += [slot_put(rid, ts, res_id, "hard") for ts in slots_covering(t, start)]
    transact(ops, "check-in conflict, try again")
    st.update(session=new, res_active=1, presence_seen_at=t)
    return room_view(rid, st, user)


def h_end(ev, user):
    rid = ev["pathParameters"]["id"]
    st = get_state(rid)
    sess = st.get("session")
    if not sess:
        return room_view(rid, st, user)
    if sess["user"] != user:
        raise ApiError(403, "only the holder can end this session")
    end_session(rid, st, "manual")
    return room_view(rid, st, user)


def qr_url(room_id):
    """What the FreeWili shows as a QR code. The device draws whatever string this is,
    so it can later carry a rotating token without a firmware change."""
    return f"{SITE_URL}/r/{room_id}"


def device_view(room_id, st):
    view = room_view(room_id, st)
    view["url"] = qr_url(room_id)
    # lock command: 1 = unlock while a session is active (held or warn), 0 = lock
    view["servo"] = 1 if view["state"] in ("held", "warn") else 0
    return view


def h_device_room(ev):
    rid = ev["pathParameters"]["id"]
    return device_view(rid, maintain(rid))


def h_device_end(ev):
    rid = ev["pathParameters"]["id"]
    st = get_state(rid)
    end_session(rid, st, "device_button")
    return device_view(rid, st)


def h_presence(ev):
    rid = ev["pathParameters"]["id"]
    present = bool(body(ev).get("present"))
    t = now()
    expr = "SET presence = :p" + (", presence_seen_at = :t" if present else "")
    values = {":p": present}  # stored as a DynamoDB Boolean
    if present:
        values[":t"] = t
    try:
        table().update_item(
            Key={"room_id": rid, "sk": "STATE"}, UpdateExpression=expr,
            ConditionExpression="attribute_exists(sk)",
            ExpressionAttributeValues=values,
        )
    except ClientError as e:
        if e.response["Error"]["Code"] == "ConditionalCheckFailedException":
            raise ApiError(404, f"room {rid} not found")
        raise
    return device_view(rid, maintain(rid))  # device gets fresh state in the same call


def h_book(ev, user):
    b = body(ev)
    try:
        rid = str(b["room_id"])
        start, end = int(b["start"]), int(b["end"])
    except (KeyError, TypeError, ValueError):
        raise ApiError(400, "room_id, start and end (epoch seconds) are required")
    if start % SLOT or end % SLOT:
        raise ApiError(400, f"start and end must be on {SLOT // 60}-minute boundaries")
    if end <= start:
        raise ApiError(400, "end must be after start")
    if start < slot_floor(now()):
        raise ApiError(400, "can't book in the past")
    n = (end - start) // SLOT
    if n > MAX_SLOTS:
        raise ApiError(400, f"max booking is {MAX_SLOTS * SLOT // 60} minutes")
    get_state(rid)  # clean 404 instead of a generic 409
    res_id = "r_" + uuid.uuid4().hex[:10]
    ops = [
        tx_check({"room_id": rid, "sk": "STATE"}, "attribute_exists(sk)"),
        tx_put({"room_id": rid, "sk": f"RES#{res_id}", "start_ts": start, "end_ts": end,
                "user": user, "checked_in": False, "status": "booked", "created_at": now()}),
    ]
    ops += [slot_put(rid, ts, res_id, "hard") for ts in slots_covering(start, end)]
    transact(ops, "that time overlaps another booking or the current session")
    return {"res_id": res_id, "room_id": rid, "start": start, "end": end, "user": user}


def find_reservation(res_id):
    """The path has no room_id, so find the row by its sort key. Fine at demo scale."""
    resp = table().scan(FilterExpression=Attr("sk").eq(f"RES#{res_id}"))
    items = resp["Items"]
    while not items and "LastEvaluatedKey" in resp:
        resp = table().scan(FilterExpression=Attr("sk").eq(f"RES#{res_id}"),
                            ExclusiveStartKey=resp["LastEvaluatedKey"])
        items = resp["Items"]
    return items[0] if items else None


def h_cancel(ev, user):
    res_id = ev["pathParameters"]["res_id"]
    r = find_reservation(res_id)
    if not r:
        raise ApiError(404, "reservation not found")
    rid = r["room_id"]
    if r["user"] != user:
        raise ApiError(403, "you can only cancel your own reservations")
    if r.get("status") == "checked_in":
        raise ApiError(409, "already checked in, use POST /rooms/{id}/end")
    ops = [tx_delete({"room_id": rid, "sk": r["sk"]})]
    for s in query_slots(rid, int(r["start_ts"]), int(r["end_ts"])):
        if s["res_id"] == res_id:
            ops.append(tx_delete({"room_id": rid, "sk": s["sk"]}))
    transact(ops, "reservation changed, try again")
    return {"cancelled": res_id}


def h_mine(ev, user):
    q = ev.get("queryStringParameters") or {}
    if q.get("mine") != "true":
        raise ApiError(400, "only ?mine=true is supported")
    items = table().scan(
        FilterExpression=Attr("sk").begins_with("RES#") & Attr("user").eq(user)
        & Attr("status").is_in(["booked", "checked_in"])
    )["Items"]
    return {"reservations": [
        {"res_id": i["sk"][4:], "room_id": i["room_id"], "start": i["start_ts"],
         "end": i["end_ts"], "status": i["status"]} for i in items]}


def load_settings():
    """Read the admin speed dial once per invocation, so a change applies to the next request."""
    try:
        item = table().get_item(Key=SETTINGS_KEY).get("Item") or {}
    except ClientError as e:  # never take the whole API down over the dial: fall back to the env default
        print(json.dumps({"settings_unreadable": str(e)}))
        item = {}
    set_speed(item.get("seconds_per_minute", DEFAULT_MINUTE))
    set_slot(item.get("slot_minutes", DEFAULT_SLOT_MINUTES))


def is_admin(ev):
    """Members of the Cognito group "admin". HTTP API JWT authorizers pass a list claim as the
    string "[admin other]", so both that and a real list are accepted; matching is exact."""
    claims = ev.get("requestContext", {}).get("authorizer", {}).get("jwt", {}).get("claims", {})
    groups = claims.get("cognito:groups") or []
    if isinstance(groups, str):
        groups = groups.strip("[]").replace(",", " ").split()
    return "admin" in groups


def room_rows(room_id):
    """Every row of one room. shortcut: one query page (1 MB), plenty at demo scale; paginate if it grows."""
    return table().query(KeyConditionExpression=Key("room_id").eq(room_id), ConsistentRead=True)["Items"]


def admin_room(st):
    """One room as the admin page sees it: session, every booking, and stray slots. A stray slot is
    a current or future SLOT# row that no live session or booking owns, so it blocks that time."""
    rid, t = st["room_id"], now()
    rows = room_rows(rid)
    sess = st.get("session") or None
    res = [r for r in rows if r["sk"].startswith("RES#")]
    live = {r["sk"][4:] for r in res if r.get("status") in ("booked", "checked_in")}
    if sess:
        live.add(sess["res_id"])
    stray = [r for r in rows if r["sk"].startswith("SLOT#") and r["res_id"] not in live
             and slot_ts(r["sk"]) >= slot_floor(t)]
    return {
        "room_id": rid, "presence": is_present(st), "session": sess,
        "reservations": sorted(({"sk": r["sk"], "user": r.get("user"), "start": r["start_ts"], "end": r["end_ts"],
                                 "status": r.get("status")} for r in res), key=lambda r: -int(r["start"])),
        "stray_slots": [{"sk": r["sk"], "start": slot_ts(r["sk"]), "res_id": r["res_id"]} for r in stray],
    }


def h_admin(ev, user):
    """Every room's session, bookings and stray slots, and the current rule durations."""
    items = table().scan(FilterExpression=Attr("sk").eq("STATE"))["Items"]
    return {
        "seconds_per_minute": MINUTE,
        "slot_minutes": SLOT // 60,
        "rules": {"extension_step": TICK, "empty_room_timeout": VOID, "reserved_lead": SOFT_LEAD,
                  "no_show": NO_SHOW, "warning": WARN, "early_checkin": EARLY_CHECKIN},
        "rooms": [admin_room(i) for i in sorted(items, key=lambda i: str(i["room_id"]))],
    }


def h_admin_end(ev, user):
    """End any room's session, the same way the room's own end paths do (slots freed too)."""
    rid = ev["pathParameters"]["id"]
    st = get_state(rid)
    if not st.get("session"):
        raise ApiError(409, f"room {rid} has no session")
    end_session(rid, st, "admin")
    print(json.dumps({"admin_end": rid, "by": user}))
    return h_admin(ev, user)


def h_admin_delete(ev, user):
    """Delete one booking (any user's, with every slot it holds) or one stray slot.
    A room's STATE row is refused: that would delete the room itself."""
    b = body(ev)
    rid, sk = str(b.get("room_id") or ""), str(b.get("sk") or "")
    if not rid or not sk.startswith(("RES#", "SLOT#")):
        raise ApiError(400, "room_id and a RES# or SLOT# sk are required; end a session instead of deleting it")
    rows = {r["sk"]: r for r in room_rows(rid)}
    row = rows.get(sk)
    if not row:
        raise ApiError(404, f"no row {sk} in room {rid}")
    owner = sk[4:] if sk.startswith("RES#") else row["res_id"]
    st = get_state(rid)
    if (st.get("session") or {}).get("res_id") == owner:
        end_session(rid, st, "admin")  # checked-in booking, or a slot of the live session: end it first
        rows = {r["sk"]: r for r in room_rows(rid)}
    if sk.startswith("RES#"):
        # shortcut: one transaction caps at 100 rows, so a booking longer than ~24 h keeps a few slots
        ops = [tx_delete({"room_id": rid, "sk": sk})] + [
            tx_delete({"room_id": rid, "sk": k}) for k, r in rows.items()
            if k.startswith("SLOT#") and r["res_id"] == owner][:99]
    else:
        ops = [tx_delete({"room_id": rid, "sk": sk})] if sk in rows else []
    if ops:
        transact(ops, "that row changed, try again")
    print(json.dumps({"admin_delete": sk, "room": rid, "by": user}))
    return h_admin(ev, user)


def live_slot_rooms():
    """Rooms holding a SLOT# row that is current or in the future, under the current slot length."""
    t, rooms, kw = now(), set(), {}
    while True:  # every page: this check guards the double-booking invariant, so it can't skip any
        resp = table().scan(FilterExpression=Attr("sk").begins_with("SLOT#"), **kw)
        rooms |= {r["room_id"] for r in resp["Items"] if slot_ts(r["sk"]) + SLOT > t}
        if "LastEvaluatedKey" not in resp:
            return sorted(rooms, key=str)
        kw = {"ExclusiveStartKey": resp["LastEvaluatedKey"]}


def h_admin_settings(ev, user):
    """Body: {"seconds_per_minute": n} and/or {"slot_minutes": n}."""
    b = body(ev)
    sets, values = [], {}
    if "seconds_per_minute" in b:
        try:
            spm = float(b["seconds_per_minute"])
        except (TypeError, ValueError):
            raise ApiError(400, "seconds_per_minute must be a number")
        if not 1 <= spm <= 60:  # below 1, the 5-minute rules round down to 0 s and sessions stop extending
            raise ApiError(400, "seconds_per_minute must be between 1 and 60")
        sets.append("seconds_per_minute = :m")
        values[":m"] = Decimal(str(spm))
    if "slot_minutes" in b:
        slot_min = b["slot_minutes"]
        if slot_min not in SLOT_CHOICES or isinstance(slot_min, bool):
            raise ApiError(400, f"slot_minutes must be one of {', '.join(map(str, SLOT_CHOICES))}")
        slot_min = int(slot_min)  # JSON 15.0 -> 15; DynamoDB rejects floats
        if slot_min * 60 != SLOT:
            # shortcut: check-then-set, so a booking made in the same instant could slip in; fine for admin use
            busy = live_slot_rooms()
            if busy:
                raise ApiError(409, "can't change the slot length while slots are taken in room "
                               + ", ".join(map(str, busy)) + ". End those sessions and delete upcoming bookings first")
        sets.append("slot_minutes = :s")
        values[":s"] = slot_min
    if not sets:
        raise ApiError(400, "send seconds_per_minute and/or slot_minutes")
    table().update_item(  # update, not put: the role already has UpdateItem for presence
        Key=SETTINGS_KEY,
        UpdateExpression="SET " + ", ".join(sets + ["updated_by = :u", "updated_at = :t"]),
        ExpressionAttributeValues={**values, ":u": user, ":t": now()},
    )
    if ":m" in values:
        set_speed(values[":m"])
    if ":s" in values:
        set_slot(values[":s"])
    print(json.dumps({"admin_settings": {k: str(v) for k, v in values.items()}, "by": user}))
    return h_admin(ev, user)


def tick():
    rooms = table().scan(FilterExpression=Attr("sk").eq("STATE"))["Items"]
    for st in rooms:
        maintain(st["room_id"], st)
    return {"ticked": len(rooms)}


# --------------------------------------------------------------------- plumbing

# Web routes: put the JWT authorizer on every one of these in API Gateway.
USER_ROUTES = {
    "GET /rooms": h_list_rooms,
    "GET /rooms/{id}": h_get_room,
    "GET /rooms/{id}/slots": h_slots,
    "POST /rooms/{id}/claim": h_claim,
    "POST /rooms/{id}/checkin": h_checkin,
    "POST /rooms/{id}/end": h_end,
    "GET /reservations": h_mine,
    "POST /reservations": h_book,
    "DELETE /reservations/{res_id}": h_cancel,
}
# Admin routes: JWT authorizer in API Gateway, plus the "admin" group check in lambda_handler.
ADMIN_ROUTES = {
    "GET /admin": h_admin,
    "POST /admin/rooms/{id}/end": h_admin_end,
    "POST /admin/settings": h_admin_settings,
    "POST /admin/delete": h_admin_delete,
}
# FreeWili routes: NO authorizer in API Gateway. The x-device-key header protects them.
DEVICE_ROUTES = {
    "GET /device/rooms/{id}": h_device_room,
    "POST /device/rooms/{id}/presence": h_presence,
    "POST /device/rooms/{id}/end": h_device_end,}


def body(ev):
    raw = ev.get("body") or "{}"
    if ev.get("isBase64Encoded"):
        import base64
        raw = base64.b64decode(raw).decode()
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        raise ApiError(400, "body must be JSON")


def get_user(ev):
    claims = ev.get("requestContext", {}).get("authorizer", {}).get("jwt", {}).get("claims", {})
    u = claims.get("cognito:username") or claims.get("username") or claims.get("sub")
    if not u and os.environ.get("ALLOW_DEV_USER") == "1":
        u = (ev.get("headers") or {}).get("x-user")
    return u


def is_device(ev):
    rid = (ev.get("pathParameters") or {}).get("id")
    key = (ev.get("headers") or {}).get("x-device-key")
    if not rid or not key:
        return False
    keys = json.loads(os.environ.get("DEVICE_KEYS", "{}"))
    expected = keys.get(str(rid))
    return bool(expected) and hmac.compare_digest(key, expected)


def _json_default(o):
    if isinstance(o, Decimal):
        return int(o) if o == o.to_integral_value() else float(o)
    raise TypeError(type(o))


def respond(status, payload):
    return {"statusCode": status, "headers": {"content-type": "application/json"},
            "body": json.dumps(payload, default=_json_default)}


def respond_error(status, msg):
    """Errors go back as plain text, e.g. "Room is already held"."""
    return {"statusCode": status, "headers": {"content-type": "text/plain"},
            "body": msg[:1].upper() + msg[1:]}


_PATTERNS = [  # used when called through a Lambda Function URL (no API Gateway routing)
    ("GET",    r"/rooms",                              "GET /rooms"),
    ("GET",    r"/rooms/(?P<id>[^/]+)",                "GET /rooms/{id}"),
    ("GET",    r"/rooms/(?P<id>[^/]+)/slots",          "GET /rooms/{id}/slots"),
    ("POST",   r"/rooms/(?P<id>[^/]+)/claim",          "POST /rooms/{id}/claim"),
    ("POST",   r"/rooms/(?P<id>[^/]+)/checkin",        "POST /rooms/{id}/checkin"),
    ("POST",   r"/rooms/(?P<id>[^/]+)/end",            "POST /rooms/{id}/end"),
    ("GET",    r"/reservations",                       "GET /reservations"),
    ("POST",   r"/reservations",                       "POST /reservations"),
    ("DELETE", r"/reservations/(?P<res_id>[^/]+)",     "DELETE /reservations/{res_id}"),
    ("GET",    r"/device/rooms/(?P<id>[^/]+)",         "GET /device/rooms/{id}"),
    ("POST",   r"/device/rooms/(?P<id>[^/]+)/presence", "POST /device/rooms/{id}/presence"),
    ("POST",   r"/device/rooms/(?P<id>[^/]+)/end",     "POST /device/rooms/{id}/end"),
    ("GET",    r"/admin",                              "GET /admin"),
    ("POST",   r"/admin/rooms/(?P<id>[^/]+)/end",      "POST /admin/rooms/{id}/end"),
    ("POST",   r"/admin/settings",                     "POST /admin/settings"),
    ("POST",   r"/admin/delete",                       "POST /admin/delete"),]


def resolve_route(event):
    """API Gateway sets routeKey; a Function URL sends "$default", so match the path."""
    route = event.get("routeKey", "")
    if route and route != "$default":
        return route
    import re
    method = event.get("requestContext", {}).get("http", {}).get("method", "")
    path = (event.get("rawPath") or "/").rstrip("/") or "/"
    for m, pat, key in _PATTERNS:
        match = re.fullmatch(pat, path)
        if m == method and match:
            event["pathParameters"] = match.groupdict()
            return key
    return f"{method} {path}"


def lambda_handler(event, context=None):
    load_settings()
    if event.get("action") == "tick":       # EventBridge Scheduler input: {"action":"tick"}
        return tick()
    route = resolve_route(event)
    try:
        if route in DEVICE_ROUTES:
            if not is_device(event):
                raise ApiError(403, "device key required")
            return respond(200, DEVICE_ROUTES[route](event))
        if route in USER_ROUTES or route in ADMIN_ROUTES:
            user = get_user(event)
            if not user:
                raise ApiError(401, "log in first")
            if route in ADMIN_ROUTES:
                if not is_admin(event):
                    raise ApiError(403, "admins only")
                return respond(200, ADMIN_ROUTES[route](event, user))
            return respond(200, USER_ROUTES[route](event, user))
        raise ApiError(404, f"no route {route}")
    except ApiError as e:
        return respond_error(e.status, e.msg)
    except Exception as e:  # anything unexpected: log the full traceback, return the cause
        import traceback
        traceback.print_exc()
        code = getattr(e, "response", {}).get("Error", {}).get("Code") if hasattr(e, "response") else None
        return respond_error(500, f"Server error on {route}: {code or type(e).__name__}: {e}")
