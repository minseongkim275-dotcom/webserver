#include "router.h"
#include "test.h"

static config make(void) {
    config c;
    char err[128];
    config_defaults(&c);
    config_apply_line(&c, "upstream api 127.0.0.1:9001 127.0.0.1:9002 127.0.0.1:9003", err, sizeof err);
    config_apply_line(&c, "upstream files 127.0.0.1:9100", err, sizeof err);
    config_apply_line(&c, "route /api api", err, sizeof err);
    config_apply_line(&c, "route /api/files/ files", err, sizeof err);
    config_apply_line(&c, "route /api/files/public static", err, sizeof err);
    config_apply_line(&c, "route /shop/ api", err, sizeof err);
    config_finish(&c, err, sizeof err);
    return c;
}

static void prefix_boundaries(void) {
    config c = make();
    CHECK_INT(router_match(&c, "/api"), 0);
    CHECK_INT(router_match(&c, "/api/"), 0);
    CHECK_INT(router_match(&c, "/api/hello"), 0);
    CHECK_INT(router_match(&c, "/apix"), -1);   /* 단어 경계가 아님 */
    CHECK_INT(router_match(&c, "/ap"), -1);
    CHECK_INT(router_match(&c, "/"), -1);
    CHECK_INT(router_match(&c, "/index.html"), -1);
    CHECK_INT(router_match(&c, "/shop/"), 0);   /* '/'로 끝나는 접두사 */
    CHECK_INT(router_match(&c, "/shop/a/b"), 0);
    CHECK_INT(router_match(&c, "/shop"), -1);   /* 접두사 "/shop/"보다 짧다 */
}

static void longest_prefix_wins(void) {
    config c = make();
    CHECK_INT(router_match(&c, "/api/files/a.txt"), 1);      /* files */
    CHECK_INT(router_match(&c, "/api/files/public"), -1);    /* 더 긴 접두사가 static */
    CHECK_INT(router_match(&c, "/api/files/public/x"), -1);
    CHECK_INT(router_match(&c, "/api/other"), 0);
}

static void round_robin_order(void) {
    config c = make();
    upstream *u = &c.ups[0];
    time_t now = 1000;
    int seq[7];
    for (int i = 0; i < 7; i++) seq[i] = router_pick(u, now);
    CHECK_INT(seq[0], 0);
    CHECK_INT(seq[1], 1);
    CHECK_INT(seq[2], 2);
    CHECK_INT(seq[3], 0);
    CHECK_INT(seq[6], 0);
}

static void down_backends_are_skipped_until_they_recover(void) {
    config c = make();
    upstream *u = &c.ups[0];
    time_t now = 1000;
    router_mark_down(u, 1, now);
    int seq[4];
    for (int i = 0; i < 4; i++) seq[i] = router_pick(u, now);
    CHECK_INT(seq[0], 0);
    CHECK_INT(seq[1], 2);
    CHECK_INT(seq[2], 0);
    CHECK_INT(seq[3], 2);

    /* 시간이 지나면 다시 후보가 된다 */
    int seen1 = 0;
    for (int i = 0; i < 3; i++)
        if (router_pick(u, now + ROUTER_DOWN_SECS) == 1) seen1 = 1;
    CHECK_INT(seen1, 1);
}

static void all_down_still_picks_something(void) {
    config c = make();
    upstream *u = &c.ups[0];
    for (int i = 0; i < 3; i++) router_mark_down(u, i, 1000);
    int a = router_pick(u, 1000), b = router_pick(u, 1000);
    CHECK(a >= 0 && a < 3);
    CHECK(b >= 0 && b < 3);
    CHECK(a != b);
}

static void single_backend(void) {
    config c = make();
    upstream *u = &c.ups[1];
    CHECK_INT(router_pick(u, 1), 0);
    CHECK_INT(router_pick(u, 1), 0);
}

int main(void) {
    RUN(prefix_boundaries);
    RUN(longest_prefix_wins);
    RUN(round_robin_order);
    RUN(down_backends_are_skipped_until_they_recover);
    RUN(all_down_still_picks_something);
    RUN(single_backend);
    TEST_DONE();
}
