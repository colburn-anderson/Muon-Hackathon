/* The room node's JSON reader, exercised on the exact shapes it will see.
 *
 * jhelp.c lives in the ESP-IDF project, but it is plain C with no IDF headers,
 * which is precisely why the edge cases are cheap enough to test here: a bug in
 * this file means the display shows "unknown" for a room that is fine, or an
 * alert banner that never clears. */

#include "test_util.h"
#include "jhelp.h"

#include <string.h>

int main(void) {
    /* The documented GET /rooms/1 response. */
    const char *room =
        "{\"room_id\":\"1\",\"state\":\"reserved\",\"holder\":\"somebody\","
        "\"url\":\"https://rooms.example/r/1?t=abc\","
        "\"expires_at\":1763200000,\"next_res_start\":1763203600}";

    char buf[64];
    ASSERT_TRUE(jh_string(room, "state", buf, sizeof buf));
    ASSERT_EQ(strcmp(buf, "reserved"), 0);

    ASSERT_TRUE(jh_string(room, "holder", buf, sizeof buf));
    ASSERT_EQ(strcmp(buf, "somebody"), 0);

    ASSERT_TRUE(jh_string(room, "url", buf, sizeof buf));
    ASSERT_EQ(strcmp(buf, "https://rooms.example/r/1?t=abc"), 0);

    double n = 0;
    ASSERT_TRUE(jh_number(room, "expires_at", &n));
    ASSERT_EQ((long long)n, 1763200000LL);

    ASSERT_TRUE(jh_number(room, "next_res_start", &n));
    ASSERT_EQ((long long)n, 1763203600LL);

    /* Device timestamps are explicitly null when they do not apply. Callers
     * initialize to zero and a failed numeric parse preserves that mapping. */
    n = 0;
    ASSERT_TRUE(!jh_number("{\"expires_at\":null,\"next_res_start\":null}",
                           "expires_at", &n));
    ASSERT_EQ((long long)n, 0);
    ASSERT_TRUE(!jh_number("{\"expires_at\":null,\"next_res_start\":null}",
                           "next_res_start", &n));

    /* Absent key must fail, not return an empty string: callers rely on the
     * difference to decide "no command" versus "command with no text". */
    ASSERT_TRUE(!jh_string(room, "absent", buf, sizeof buf));
    ASSERT_TRUE(!jh_has(room, "absent"));

    /* A key name that appears only as another key's *value* must not match.
     * This is the bug a strstr-based reader ships with. */
    ASSERT_TRUE(!jh_string("{\"holder\":\"state\"}", "state", buf, sizeof buf));
    ASSERT_TRUE(!jh_has("{\"holder\":\"state\"}", "state"));

    /* Nested alert object, our assumed command shape. */
    const char *with_alert =
        "{\"state\":\"warn\",\"servo\":42,"
        "\"alert\":{\"text\":\"Reservation ends in 5 min\",\"level\":2}}";

    const char *alert = jh_object(with_alert, "alert");
    ASSERT_TRUE(alert != NULL);
    ASSERT_TRUE(jh_string(alert, "text", buf, sizeof buf));
    ASSERT_EQ(strcmp(buf, "Reservation ends in 5 min"), 0);
    ASSERT_TRUE(jh_number(alert, "level", &n));
    ASSERT_EQ((long long)n, 2);

    /* The nested object's keys must not leak into the top level. */
    ASSERT_TRUE(!jh_has(with_alert, "text"));

    ASSERT_TRUE(jh_number(with_alert, "servo", &n));
    ASSERT_EQ((long long)n, 42);

    /* Explicit null clears a banner; it must read as "present, false". */
    bool b = true;
    ASSERT_TRUE(jh_bool("{\"alert\":null}", "alert", &b));
    ASSERT_TRUE(!b);

    /* The main CPU's presence message. */
    ASSERT_TRUE(jh_bool("{\"t\":\"presence\",\"v\":true}", "v", &b));
    ASSERT_TRUE(b);
    ASSERT_TRUE(jh_string("{\"t\":\"presence\",\"v\":false}", "t", buf, sizeof buf));
    ASSERT_EQ(strcmp(buf, "presence"), 0);

    /* Escaped quotes survive, so a reason string with a quote cannot break the
     * line for the other CPU. */
    ASSERT_TRUE(jh_string("{\"text\":\"say \\\"hi\\\"\"}", "text", buf, sizeof buf));
    ASSERT_EQ(strcmp(buf, "say \"hi\""), 0);

    /* Truncation must not overflow: a tiny output buffer yields a prefix. */
    char tiny[4];
    ASSERT_TRUE(jh_string(room, "state", tiny, sizeof tiny));
    ASSERT_EQ(strlen(tiny), 3);
    ASSERT_EQ(strcmp(tiny, "res"), 0);

    /* Structurally broken input fails cleanly instead of reading out of
     * bounds. */
    ASSERT_TRUE(!jh_string("not json at all", "state", buf, sizeof buf));
    ASSERT_TRUE(!jh_string("{\"state\":", "state", buf, sizeof buf));
    ASSERT_TRUE(!jh_string("", "state", buf, sizeof buf));
    ASSERT_TRUE(!jh_has("{}", "state"));

    /* Whitespace between key, colon and value is legal JSON. */
    ASSERT_TRUE(jh_string("{ \"state\" : \"free\" }", "state", buf, sizeof buf));
    ASSERT_EQ(strcmp(buf, "free"), 0);

    TEST_RETURN();
}
