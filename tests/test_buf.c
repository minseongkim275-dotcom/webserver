#include "buf.h"
#include "test.h"

#include <stdlib.h>

static void append_and_consume(void) {
    buf b = BUF_INIT;
    CHECK_INT(buf_append_str(&b, "hello "), 0);
    CHECK_INT(buf_printf(&b, "%s %d", "world", 42), 0);
    CHECK_INT(buf_size(&b), 14);
    CHECK(memcmp(buf_ptr(&b), "hello world 42", 14) == 0);

    buf_consume(&b, 6);
    CHECK_INT(buf_size(&b), 8);
    CHECK(memcmp(buf_ptr(&b), "world 42", 8) == 0);

    buf_consume(&b, 100); /* 남은 것보다 많이 소비하면 비운다 */
    CHECK_INT(buf_size(&b), 0);
    CHECK_INT(b.off, 0);
    buf_free(&b);
    CHECK(b.data == NULL);
}

static void grows_past_initial_capacity(void) {
    buf b = BUF_INIT;
    char chunk[1000];
    memset(chunk, 'x', sizeof chunk);
    for (int i = 0; i < 100; i++) CHECK_INT(buf_append(&b, chunk, sizeof chunk), 0);
    CHECK_INT(buf_size(&b), 100000);
    CHECK(b.cap >= 100000);
    buf_free(&b);
}

static void reuses_consumed_space_before_growing(void) {
    buf b = BUF_INIT;
    char chunk[200];
    memset(chunk, 'a', sizeof chunk);
    buf_append(&b, chunk, sizeof chunk); /* cap 256 */
    size_t cap = b.cap;
    buf_consume(&b, 150);
    memset(chunk, 'b', sizeof chunk);
    CHECK_INT(buf_append(&b, chunk, 150), 0); /* 50 + 150 <= 256: 당기기만 한다 */
    CHECK_INT(b.cap, cap);
    CHECK_INT(buf_size(&b), 200);
    CHECK(buf_ptr(&b)[0] == 'a' && buf_ptr(&b)[49] == 'a' && buf_ptr(&b)[50] == 'b');
    buf_free(&b);
}

int main(void) {
    RUN(append_and_consume);
    RUN(grows_past_initial_capacity);
    RUN(reuses_consumed_space_before_growing);
    TEST_DONE();
}
