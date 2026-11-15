"""Host tests for the admin additions: no AWS, no boto3. Run from roomlock-api/: python3 -m unittest test_api"""
import json
import sys
import types
import unittest


class _Expr:
    """Stands in for boto3 condition builders: Attr("sk").eq("STATE") & ... just builds a value."""

    def __init__(self, *a):
        self.a = a

    def __getattr__(self, name):
        return lambda *a: _Expr(name, *a)

    def __and__(self, other):
        return _Expr("and", self, other)


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
        self.meta = types.SimpleNamespace(client=types.SimpleNamespace(
            transact_write_items=lambda TransactItems: self.tx.append(TransactItems)))

    def get_item(self, Key, ConsistentRead=False):
        if self.fail_get:
            raise ClientError("AccessDeniedException")
        item = self.items.get((Key["room_id"], Key["sk"]))
        return {"Item": dict(item)} if item else {}

    def update_item(self, Key, UpdateExpression, ExpressionAttributeValues=None, **kw):
        assert UpdateExpression.startswith("SET seconds_per_minute"), UpdateExpression
        v = ExpressionAttributeValues
        self.items[(Key["room_id"], Key["sk"])] = {
            **Key, "seconds_per_minute": v[":m"], "updated_by": v[":u"], "updated_at": v[":t"]}

    def scan(self, FilterExpression=None):
        return {"Items": [dict(i) for i in self.items.values() if i["sk"] == "STATE"]}

    def query(self, **kw):
        return {"Items": []}  # no SLOT rows in these tests


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
