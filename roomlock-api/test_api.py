"""Host tests for the admin additions: no AWS, no boto3. Run from roomlock-api/: python3 -m unittest test_api"""
import json
import sys
import types
import unittest


class _Expr:
    """Stands in for boto3 condition builders: Key("sk").between(a, b) builds a tree that
    FakeTable can evaluate. Key("x") is ("x",); .op(args) is (op, parent, *args)."""

    def __init__(self, *a):
        self.a = a

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        return lambda *args: _Expr(name, self, *args)

    def __and__(self, other):
        return _Expr("and", self, other)


def matches(item, e):
    op = e.a[0]
    if op == "and":
        return matches(item, e.a[1]) and matches(item, e.a[2])
    v = item.get(e.a[1].a[0])
    if op == "eq":
        return v == e.a[2]
    if op == "between":
        return v is not None and e.a[2] <= v <= e.a[3]
    if op == "begins_with":
        return str(v).startswith(e.a[2])
    if op == "is_in":
        return v in e.a[2]
    raise NotImplementedError(op)


class ClientError(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.response = {"Error": {"Code": code}}


# Stub boto3/botocore before importing the Lambda.
_conditions = types.ModuleType("boto3.dynamodb.conditions")
_conditions.Attr = _conditions.Key = _Expr
_exceptions = types.ModuleType("botocore.exceptions")
_exceptions.ClientError = ClientError
sys.modules.update({
    "boto3": types.ModuleType("boto3"),
    "boto3.dynamodb": types.ModuleType("boto3.dynamodb"),
    "boto3.dynamodb.conditions": _conditions,
    "botocore": types.ModuleType("botocore"),
    "botocore.exceptions": _exceptions,
})

import lambda_function as api  # noqa: E402


class FakeTable:
    """Just enough of a DynamoDB table for the admin paths."""

    def __init__(self, items=(), fail_get=False):
        self.items = {(i["room_id"], i["sk"]): dict(i) for i in items}
        self.fail_get, self.tx = fail_get, []
        self.meta = types.SimpleNamespace(client=types.SimpleNamespace(transact_write_items=self._transact))

    def _transact(self, TransactItems):
        """Applies the writes the admin paths make: deletes, and end_session's two updates."""
        self.tx.append(TransactItems)
        for op in TransactItems:
            kind, body = next(iter(op.items()))
            key = (body["Key"]["room_id"], body["Key"]["sk"])
            if kind == "Delete":
                self.items.pop(key, None)
            elif kind == "Update" and "REMOVE #s" in body["UpdateExpression"]:
                self.items[key].pop("session", None)
            elif kind == "Update" and ":done" in body["ExpressionAttributeValues"]:
                self.items[key]["status"] = "ended"

    def get_item(self, Key, ConsistentRead=False):
        if self.fail_get:
            raise ClientError("AccessDeniedException")
        item = self.items.get((Key["room_id"], Key["sk"]))
        return {"Item": dict(item)} if item else {}

    def update_item(self, Key, UpdateExpression, ExpressionAttributeValues=None, **kw):
        """Only the settings write uses this here: "SET a = :x, b = :y"."""
        assert UpdateExpression.startswith("SET "), UpdateExpression
        item = self.items.setdefault((Key["room_id"], Key["sk"]), dict(Key))
        for part in UpdateExpression[4:].split(","):
            name, ref = (x.strip() for x in part.split("="))
            item[name] = ExpressionAttributeValues[ref]

    def scan(self, FilterExpression, ExclusiveStartKey=None):
        return {"Items": [dict(i) for i in self.items.values() if matches(i, FilterExpression)]}

    def query(self, KeyConditionExpression, ConsistentRead=False):
        rows = [dict(i) for i in self.items.values() if matches(i, KeyConditionExpression)]
        return {"Items": sorted(rows, key=lambda i: i["sk"])}


SESSION = {"res_id": "s_abc", "user": "someone", "kind": "soft", "started_at": 1, "expires_at": 2}


def event(route, groups="[admin]", body=None, **path):
    claims = {"cognito:username": "u1"}
    if groups is not None:
        claims["cognito:groups"] = groups
    return {"routeKey": route, "pathParameters": path or None, "body": json.dumps(body) if body else None,
            "requestContext": {"authorizer": {"jwt": {"claims": claims}}}}


class Admin(unittest.TestCase):
    def setUp(self):
        self.table = FakeTable([
            {"room_id": "1", "sk": "STATE", "presence": False, "session": dict(SESSION)},
            {"room_id": "2", "sk": "STATE", "presence": True},
        ])
        api._table = self.table

    def tearDown(self):
        api._table = None
        api.set_speed(60)
        api.set_slot(15)

    def call(self, *a, **kw):
        out = api.lambda_handler(event(*a, **kw))
        try:
            return out["statusCode"], json.loads(out["body"])
        except json.JSONDecodeError:
            return out["statusCode"], out["body"]

    def test_only_the_admin_group_gets_in(self):
        self.assertEqual(self.call("GET /admin", groups=None)[0], 403)
        self.assertEqual(self.call("GET /admin", groups="[administrators]")[0], 403)  # exact match only
        self.assertEqual(self.call("GET /admin", groups="[students admin]")[0], 200)
        self.assertEqual(self.call("GET /admin", groups=["admin"])[0], 200)

    def test_admin_view_lists_sessions_and_rules(self):
        status, out = self.call("GET /admin")
        self.assertEqual(status, 200)
        self.assertEqual([(r["room_id"], r["session"]) for r in out["rooms"]], [("1", SESSION), ("2", None)])
        self.assertEqual(out["rules"]["no_show"], 600)

    def add(self, room, sk, **f):
        self.table.items[(room, sk)] = {"room_id": room, "sk": sk, **f}

    def bookings(self):
        """Room 2: a booked reservation with two slots, a stray future slot, a stray past slot."""
        t = api.slot_floor(api.now())
        self.add("2", "RES#r_1", user="ann", start_ts=t + 3600, end_ts=t + 5400, status="booked")
        self.add("2", "RES#r_old", user="bob", start_ts=t - 7200, end_ts=t - 6300, status="no_show")
        for ts in (t + 3600, t + 4500):
            self.add("2", api.slot_sk(ts), res_id="r_1", kind="hard")
        self.add("2", api.slot_sk(t + 900), res_id="s_gone", kind="soft")
        self.add("2", api.slot_sk(t - 900), res_id="s_gone", kind="soft")
        return t

    def test_admin_view_shows_every_booking_and_only_blocking_stray_slots(self):
        t = self.bookings()
        room = self.call("GET /admin")[1]["rooms"][1]
        self.assertEqual([r["sk"] for r in room["reservations"]], ["RES#r_1", "RES#r_old"])  # newest first
        self.assertEqual(room["stray_slots"], [{"sk": api.slot_sk(t + 900), "start": t + 900, "res_id": "s_gone"}])

    def test_delete_any_users_booking_with_its_slots(self):
        t = self.bookings()
        status, _ = self.call("POST /admin/delete", body={"room_id": "2", "sk": "RES#r_1"})
        self.assertEqual(status, 200)
        left = sorted(sk for room, sk in self.table.items if room == "2")
        self.assertEqual(left, sorted(["STATE", "RES#r_old", api.slot_sk(t + 900), api.slot_sk(t - 900)]))

    def test_delete_a_checked_in_booking_ends_its_session_first(self):
        t = api.slot_floor(api.now())
        self.table.items[("1", "STATE")]["session"] = {**SESSION, "res_id": "r_9", "kind": "hard"}
        self.add("1", "RES#r_9", user="ann", start_ts=t, end_ts=t + 900, status="checked_in")
        self.add("1", api.slot_sk(t), res_id="r_9", kind="hard")
        self.assertEqual(self.call("POST /admin/delete", body={"room_id": "1", "sk": "RES#r_9"})[0], 200)
        self.assertNotIn("session", self.table.items[("1", "STATE")])
        self.assertEqual([sk for room, sk in self.table.items if room == "1"], ["STATE"])

    def test_delete_a_stray_slot(self):
        t = self.bookings()
        stray = api.slot_sk(t + 900)
        self.assertEqual(self.call("POST /admin/delete", body={"room_id": "2", "sk": stray})[0], 200)
        self.assertNotIn(("2", stray), self.table.items)
        self.assertIn(("2", "RES#r_1"), self.table.items)

    def test_delete_refuses_the_room_itself_and_unknown_rows(self):
        self.assertEqual(self.call("POST /admin/delete", body={"room_id": "1", "sk": "STATE"})[0], 400)
        self.assertEqual(self.call("POST /admin/delete", body={"room_id": "1", "sk": "RES#nope"}),
                         (404, "No row RES#nope in room 1"))
        self.assertIn(("1", "STATE"), self.table.items)

    def test_end_any_session_removes_it_even_if_not_yours(self):
        status, _ = self.call("POST /admin/rooms/{id}/end", id="1")
        self.assertEqual(status, 200)
        update = self.table.tx[0][0]["Update"]
        self.assertEqual(update["Key"], {"room_id": "1", "sk": "STATE"})
        self.assertIn("REMOVE #s", update["UpdateExpression"])
        self.assertEqual(update["ExpressionAttributeValues"][":rid"], "s_abc")

    def test_ending_a_room_with_no_session_says_so(self):
        self.assertEqual(self.call("POST /admin/rooms/{id}/end", id="2"), (409, "Room 2 has no session"))

    def test_speed_dial_sets_every_rule_and_persists(self):
        status, out = self.call("POST /admin/settings", body={"seconds_per_minute": 3})
        self.assertEqual(status, 200)
        self.assertEqual(out["rules"], {"extension_step": 15, "empty_room_timeout": 30, "reserved_lead": 30,
                                        "no_show": 30, "warning": 15, "early_checkin": 15})
        api.set_speed(60)  # a fresh Lambda container reads the stored row back
        self.assertEqual(self.call("GET /admin")[1]["rules"]["no_show"], 30)

    def test_speed_dial_rejects_bad_values(self):
        for bad in [{"seconds_per_minute": 0.5}, {"seconds_per_minute": 61}, {"seconds_per_minute": "fast"},
                    {"seconds_per_minute": "nan"}, {}]:
            self.assertEqual(self.call("POST /admin/settings", body=bad)[0], 400, bad)
        self.assertNotIn(("_admin", "SETTINGS"), self.table.items)

    def test_slot_length_can_change_when_nothing_is_booked(self):
        t = api.slot_floor(api.now())
        self.add("3", api.slot_sk(t - 3600), res_id="s_old", kind="soft")  # past rows don't block
        status, out = self.call("POST /admin/settings", body={"slot_minutes": 1})
        self.assertEqual((status, out["slot_minutes"], api.SLOT), (200, 1, 60))
        api.set_slot(15)  # a fresh container reads it back
        self.assertEqual(self.call("GET /admin")[1]["slot_minutes"], 1)
        rooms = api.lambda_handler(event("GET /rooms"))
        self.assertEqual(json.loads(rooms["body"])["slot_seconds"], 60)
        self.assertEqual(json.loads(rooms["body"])["rooms"][0]["slot_seconds"], 60)

    def test_slot_length_change_is_refused_while_slots_are_taken(self):
        t = self.bookings()  # room 2 has a booking and a stray slot ahead
        status, text = self.call("POST /admin/settings", body={"slot_minutes": 5})
        self.assertEqual(status, 409)
        self.assertIn("room 2", text)
        self.assertEqual(api.SLOT, 900)
        self.assertNotIn(("_admin", "SETTINGS"), self.table.items)
        self.assertEqual(self.call("POST /admin/settings", body={"slot_minutes": 15})[0], 200)  # no change is fine
        self.assertIn(("2", api.slot_sk(t + 900)), self.table.items)

    def test_slot_length_rejects_lengths_that_do_not_divide_an_hour(self):
        for bad in [7, 0, 90, "15", True, None]:
            self.assertEqual(self.call("POST /admin/settings", body={"slot_minutes": bad})[0], 400, bad)

    def test_booking_errors_name_the_current_slot_length(self):
        self.add("_admin", "SETTINGS", slot_minutes=5)  # every request reads the setting from the table
        t = api.slot_floor(api.now()) + 3600
        out = api.lambda_handler(event("POST /reservations", body={"room_id": "2", "start": t + 60, "end": t + 600}))
        self.assertEqual((out["statusCode"], out["body"]), (400, "Start and end must be on 5-minute boundaries"))

    def test_unreadable_settings_fall_back_instead_of_breaking_the_api(self):
        api._table = FakeTable(fail_get=True)
        api.set_speed(3)
        api.load_settings()
        self.assertEqual(api.NO_SHOW, 600)

    def test_non_admin_routes_are_unchanged(self):
        self.assertEqual(self.call("GET /nope")[0], 404)
        out = api.lambda_handler({"routeKey": "GET /rooms", "requestContext": {}})
        self.assertEqual(out["statusCode"], 401)


if __name__ == "__main__":
    unittest.main()
