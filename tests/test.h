/* 아주 작은 단위 테스트 도구. 테스트 파일 하나가 실행 파일 하나가 된다.
 * 실패해도 멈추지 않고 끝까지 돌린 뒤, 실패가 하나라도 있으면 종료 코드 1을 돌려준다. */
#ifndef WS_TEST_H
#define WS_TEST_H

#include <stdio.h>
#include <string.h>

static int t_run, t_fail;

#define T_FAIL_AT(...)                                       \
    do {                                                     \
        t_fail++;                                            \
        fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__);                        \
        fputc('\n', stderr);                                 \
    } while (0)

#define CHECK(cond)                                   \
    do {                                              \
        t_run++;                                      \
        if (!(cond)) T_FAIL_AT("CHECK(%s)", #cond);  \
    } while (0)

#define CHECK_INT(actual, expected)                                               \
    do {                                                                          \
        long long t_a_ = (long long)(actual), t_e_ = (long long)(expected);       \
        t_run++;                                                                  \
        if (t_a_ != t_e_) T_FAIL_AT("%s == %lld, want %lld", #actual, t_a_, t_e_); \
    } while (0)

#define CHECK_STR(actual, expected)                                                   \
    do {                                                                              \
        const char *t_a_ = (actual), *t_e_ = (expected);                              \
        t_run++;                                                                      \
        if (!t_a_ || strcmp(t_a_, t_e_) != 0)                                         \
            T_FAIL_AT("%s == \"%s\", want \"%s\"", #actual, t_a_ ? t_a_ : "(null)", t_e_); \
    } while (0)

/* 문자열 안에 부분 문자열이 있는지 */
#define CHECK_HAS(hay, needle)                                                   \
    do {                                                                         \
        const char *t_h_ = (hay), *t_n_ = (needle);                              \
        t_run++;                                                                 \
        if (!t_h_ || !strstr(t_h_, t_n_))                                        \
            T_FAIL_AT("%s does not contain \"%s\"", #hay, t_n_);                 \
    } while (0)

#define RUN(fn)                     \
    do {                            \
        int t_before_ = t_fail;     \
        fn();                       \
        if (t_fail != t_before_) fprintf(stderr, "  in %s\n", #fn); \
    } while (0)

#define TEST_DONE()                                                             \
    do {                                                                        \
        printf("%-24s %d checks, %d failed\n", __FILE__, t_run, t_fail);         \
        return t_fail ? 1 : 0;                                                  \
    } while (0)

#endif
