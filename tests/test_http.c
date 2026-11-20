#include "http.h"
#include "test.h"

#include <stdlib.h>

/* 요청 하나를 파싱해서 상태와 오류 코드를 돌려준다. OK이면 req를 채운 채로 둔다 */
static http_parse_status parse(const char *s, http_request *req, int *err) {
    *err = 0;
    return http_parse_request(s, strlen(s), req, err);
}

/* 오류가 나야 하는 요청: 기대한 상태 코드인지 본다 */
static void expect_error(const char *s, int want, int line) {
    http_request req;
    int err;
    http_parse_status st = parse(s, &req, &err);
    t_run++;
    if (st != HTTP_PARSE_ERROR || err != want) {
        t_fail++;
        fprintf(stderr, "  FAIL %s:%d: status=%d err=%d, want ERROR %d\n", __FILE__, line, st, err, want);
        if (st == HTTP_PARSE_OK) http_request_free(&req);
    }
}
#define EXPECT_ERROR(s, code) expect_error((s), (code), __LINE__)

static void simple_get(void) {
    http_request req;
    int err;
    const char *s = "GET /index.html?a=1&b=2 HTTP/1.1\r\nHost: localhost:8080\r\nUser-Agent: t\r\n\r\n";
    CHECK_INT(parse(s, &req, &err), HTTP_PARSE_OK);
    CHECK_STR(req.method_str, "GET");
    CHECK_INT(req.method, HTTP_M_GET);
    CHECK_STR(req.target, "/index.html?a=1&b=2");
    CHECK_STR(req.path, "/index.html");
    CHECK_STR(req.query, "a=1&b=2");
    CHECK_INT(req.version, 11);
    CHECK_STR(req.host, "localhost:8080");
    CHECK_INT(req.keep_alive, 1);
    CHECK_INT(req.content_length, -1);
    CHECK_INT(req.nheaders, 2);
    CHECK_INT(req.head_len, strlen(s));
    http_request_free(&req);
}

static void incomplete_until_blank_line(void) {
    const char *s = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        http_request req;
        int err = 0;
        http_parse_status st = http_parse_request(s, i, &req, &err);
        CHECK_INT(st, HTTP_PARSE_INCOMPLETE);
    }
    http_request req;
    int err;
    CHECK_INT(http_parse_request(s, n, &req, &err), HTTP_PARSE_OK);
    CHECK(req.query == NULL);
    http_request_free(&req);
}

static void head_len_excludes_body_and_next_request(void) {
    http_request req;
    int err;
    const char *head = "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n";
    char s[256];
    snprintf(s, sizeof s, "%shelloGET / HTTP/1.1\r\n", head);
    CHECK_INT(parse(s, &req, &err), HTTP_PARSE_OK);
    CHECK_INT(req.method, HTTP_M_POST);
    CHECK_INT(req.content_length, 5);
    CHECK_INT(req.head_len, strlen(head));
    http_request_free(&req);
}

static void headers_are_case_insensitive_and_trimmed(void) {
    http_request req;
    int err;
    const char *s = "GET / HTTP/1.1\r\nhOsT:   example.com  \r\nX-Empty:\r\nX-Tab:\tv\t\r\n\r\n";
    CHECK_INT(parse(s, &req, &err), HTTP_PARSE_OK);
    CHECK_STR(http_header_get(&req, "HOST"), "example.com");
    CHECK_STR(http_header_get(&req, "x-empty"), "");
    CHECK_STR(http_header_get(&req, "X-Tab"), "v");
    CHECK(http_header_get(&req, "Missing") == NULL);
    http_request_free(&req);
}

static void keep_alive_rules(void) {
    http_request req;
    int err;
    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &req, &err), HTTP_PARSE_OK);
    CHECK_INT(req.keep_alive, 0);
    http_request_free(&req);

    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, CLOSE\r\n\r\n", &req, &err), HTTP_PARSE_OK);
    CHECK_INT(req.keep_alive, 0);
    http_request_free(&req);

    CHECK_INT(parse("GET / HTTP/1.0\r\n\r\n", &req, &err), HTTP_PARSE_OK);
    CHECK_INT(req.version, 10);
    CHECK_INT(req.keep_alive, 0);
    CHECK(req.host == NULL);
    http_request_free(&req);

    CHECK_INT(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &req, &err), HTTP_PARSE_OK);
    CHECK_INT(req.keep_alive, 1);
    http_request_free(&req);
}

static void multiple_connection_headers_are_combined(void) {
    http_request req;
    int err;
    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n", &req, &err),
              HTTP_PARSE_OK);
    CHECK_INT(req.keep_alive, 0);
    http_request_free(&req);

    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: upgrade\r\nconnection: Close\r\n\r\n", &req, &err),
              HTTP_PARSE_OK);
    CHECK_INT(req.keep_alive, 0);
    http_request_free(&req);

    CHECK_INT(parse("GET / HTTP/1.0\r\nConnection: foo\r\nConnection: keep-alive\r\n\r\n", &req, &err),
              HTTP_PARSE_OK);
    CHECK_INT(req.keep_alive, 1);
    http_request_free(&req);

    CHECK_INT(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n", &req, &err),
              HTTP_PARSE_OK);
    CHECK_INT(req.keep_alive, 0);
    http_request_free(&req);
}

/* 요청 라인이 정확히 한도(8192)인 요청을 어디서 끊어 받아도 같은 결과여야 한다 */
static void request_line_limit_is_independent_of_chunking(void) {
    size_t line_len = HTTP_MAX_REQUEST_LINE;
    char *s = malloc(line_len + 64);
    size_t n = 0;
    memcpy(s, "GET /", 5);
    n = 5;
    size_t pad = line_len - 5 - strlen(" HTTP/1.1");
    memset(s + n, 'a', pad);
    n += pad;
    n += (size_t)sprintf(s + n, " HTTP/1.1");
    CHECK_INT(n, line_len);
    n += (size_t)sprintf(s + n, "\r\nHost: x\r\n\r\n");

    http_request req;
    int err = 0;
    CHECK_INT(http_parse_request(s, n, &req, &err), HTTP_PARSE_OK); /* 한 번에 */
    http_request_free(&req);
    for (size_t cut = line_len - 2; cut <= line_len + 3; cut++) {   /* '\r' 앞뒤로 끊기 */
        err = 0;
        http_parse_status st = http_parse_request(s, cut, &req, &err);
        CHECK_INT(st, HTTP_PARSE_INCOMPLETE);
        CHECK_INT(err, 0);
    }
    /* 한 바이트 더 길면 414 */
    char *t = malloc(n + 8);
    memcpy(t, s, 5);
    t[5] = 'b';
    memcpy(t + 6, s + 5, n - 5);
    err = 0;
    CHECK_INT(http_parse_request(t, line_len + 1, &req, &err), HTTP_PARSE_ERROR);
    CHECK_INT(err, 414);
    free(t);
    free(s);
}

static void tokens(void) {
    CHECK(http_has_token("keep-alive", "keep-alive"));
    CHECK(http_has_token(" a ,b,  Close ", "close"));
    CHECK(!http_has_token("closed", "close"));
    CHECK(!http_has_token("", "close"));
    CHECK(!http_has_token(NULL, "close"));
}

static void accepts_absolute_form_and_leading_blank_lines(void) {
    http_request req;
    int err;
    CHECK_INT(parse("GET http://example.com:81/a/b?x=1 HTTP/1.1\r\nHost: example.com\r\n\r\n", &req, &err), HTTP_PARSE_OK);
    CHECK_STR(req.path, "/a/b");
    CHECK_STR(req.query, "x=1");
    http_request_free(&req);

    CHECK_INT(parse("GET http://example.com HTTP/1.1\r\nHost: example.com\r\n\r\n", &req, &err), HTTP_PARSE_OK);
    CHECK_STR(req.path, "/");
    http_request_free(&req);

    const char *s = "\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK_INT(parse(s, &req, &err), HTTP_PARSE_OK);
    CHECK_STR(req.method_str, "GET");
    CHECK_INT(req.head_len, strlen(s));
    http_request_free(&req);
}

static void duplicate_equal_content_length_is_ok(void) {
    http_request req;
    int err;
    CHECK_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\ncontent-length: 3\r\n\r\n", &req, &err),
              HTTP_PARSE_OK);
    CHECK_INT(req.content_length, 3);
    http_request_free(&req);
}

static void malformed_requests(void) {
    EXPECT_ERROR("GET / HTTP/1.1\r\n\r\n", 400);                       /* 1.1인데 Host 없음 */
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);   /* Host 두 개 */
    EXPECT_ERROR("GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400);             /* 공백 두 개 */
    EXPECT_ERROR("GET /\r\nHost: x\r\n\r\n", 400);                       /* 버전 없음 */
    EXPECT_ERROR("GET / HTTX/1.1\r\nHost: x\r\n\r\n", 400);
    EXPECT_ERROR("GET / HTTP/1.1x\r\nHost: x\r\n\r\n", 400);
    EXPECT_ERROR("G(T / HTTP/1.1\r\nHost: x\r\n\r\n", 400);              /* 메서드에 허용되지 않는 문자 */
    EXPECT_ERROR("GET index.html HTTP/1.1\r\nHost: x\r\n\r\n", 400);     /* '/'로 시작하지 않음 */
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost x\r\n\r\n", 400);               /* 콜론 없음 */
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400);             /* 이름 뒤 공백 */
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost: x\r\nX-A: 1\r\n  folded\r\n\r\n", 400); /* obs-fold */
    EXPECT_ERROR("GET / HTTP/1.1\r\nHost: x\nX-A: 1\r\n\r\n", 400);      /* 홀로 쓴 LF */
    EXPECT_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 12a\r\n\r\n", 400);
    EXPECT_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400);
    EXPECT_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n", 400);
    EXPECT_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999\r\n\r\n", 400);
    EXPECT_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 400);
}

static void nul_byte_in_head_is_rejected(void) {
    const char s[] = "GET /a\0b HTTP/1.1\r\nHost: x\r\n\r\n";
    http_request req;
    int err = 0;
    CHECK_INT(http_parse_request(s, sizeof s - 1, &req, &err), HTTP_PARSE_ERROR);
    CHECK_INT(err, 400);
}

static void unsupported_things(void) {
    EXPECT_ERROR("BREW /pot HTTP/1.1\r\nHost: x\r\n\r\n", 501);  /* 형식은 맞지만 모르는 메서드 */
    EXPECT_ERROR("get / HTTP/1.1\r\nHost: x\r\n\r\n", 501);      /* 메서드는 대소문자를 가린다 */
    EXPECT_ERROR("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
    EXPECT_ERROR("GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505);
    EXPECT_ERROR("GET / HTTP/0.9\r\nHost: x\r\n\r\n", 505);
}

static void future_minor_version_is_treated_as_1_1(void) {
    http_request req;
    int err;
    CHECK_INT(parse("GET / HTTP/1.2\r\nHost: x\r\n\r\n", &req, &err), HTTP_PARSE_OK);
    CHECK_INT(req.version, 11);
    http_request_free(&req);
}

static void size_limits(void) {
    /* 요청 라인이 한도를 넘으면 머리가 끝나기 전이라도 414 */
    size_t n = HTTP_MAX_REQUEST_LINE + 100;
    char *s = malloc(n + 1);
    memcpy(s, "GET /", 5);
    memset(s + 5, 'a', n - 5);
    s[n] = '\0';
    EXPECT_ERROR(s, 414);

    /* 요청 라인은 짧고 헤더가 한도를 넘으면 431 */
    n = HTTP_MAX_HEAD + 100;
    s = realloc(s, n + 1);
    int k = snprintf(s, n + 1, "GET / HTTP/1.1\r\nHost: x\r\nX-Big: ");
    memset(s + k, 'b', n - (size_t)k);
    s[n] = '\0';
    EXPECT_ERROR(s, 431);
    free(s);

    /* 헤더 개수 한도 */
    buf b = BUF_INIT;
    buf_append_str(&b, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS; i++) buf_printf(&b, "X-H%d: v\r\n", i);
    buf_append_str(&b, "\r\n");
    buf_append(&b, "", 1);
    EXPECT_ERROR(buf_ptr(&b), 431);
    buf_free(&b);
}

static void response_head(void) {
    http_response_info info;
    const char *s = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nhello world!";
    CHECK_INT(http_parse_response_head(s, strlen(s), &info), HTTP_PARSE_OK);
    CHECK_INT(info.status, 200);
    CHECK_INT(info.version, 11);
    CHECK_INT(info.content_length, 12);
    CHECK_INT(info.chunked, 0);
    CHECK_INT(info.head_len, strlen(s) - 12);

    s = "HTTP/1.0 404\r\nTransfer-Encoding: gzip, chunked\r\n\r\n";
    CHECK_INT(http_parse_response_head(s, strlen(s), &info), HTTP_PARSE_OK);
    CHECK_INT(info.status, 404);
    CHECK_INT(info.version, 10);
    CHECK_INT(info.content_length, -1);
    CHECK_INT(info.chunked, 1);

    s = "HTTP/1.1 200 OK\r\nContent-Le";
    CHECK_INT(http_parse_response_head(s, strlen(s), &info), HTTP_PARSE_INCOMPLETE);

    s = "FOO/1.1 200 OK\r\n\r\n";
    CHECK_INT(http_parse_response_head(s, strlen(s), &info), HTTP_PARSE_ERROR);
    s = "HTTP/1.1 20 OK\r\n\r\n";
    CHECK_INT(http_parse_response_head(s, strlen(s), &info), HTTP_PARSE_ERROR);
    s = "HTTP/1.1 200 OK\r\nContent-Length: x\r\n\r\n";
    CHECK_INT(http_parse_response_head(s, strlen(s), &info), HTTP_PARSE_ERROR);
}

static void status_text(void) {
    CHECK_STR(http_status_text(200), "OK");
    CHECK_STR(http_status_text(404), "Not Found");
    CHECK_STR(http_status_text(414), "URI Too Long");
    CHECK_STR(http_status_text(504), "Gateway Timeout");
    CHECK_STR(http_status_text(299), "Unknown");
}

static void writes_head(void) {
    buf b = BUF_INIT;
    CHECK_INT(http_write_head(&b, 301, "text/html", 0, 1, "Location: /dir/\r\n"), 0);
    buf_append(&b, "", 1);
    const char *s = buf_ptr(&b);
    CHECK(strncmp(s, "HTTP/1.1 301 Moved Permanently\r\n", 32) == 0);
    CHECK_HAS(s, "\r\nContent-Type: text/html\r\n");
    CHECK_HAS(s, "\r\nContent-Length: 0\r\n");
    CHECK_HAS(s, "\r\nConnection: keep-alive\r\n");
    CHECK_HAS(s, "\r\nLocation: /dir/\r\n");
    CHECK_HAS(s, "\r\nDate: ");
    CHECK(strcmp(s + strlen(s) - 4, "\r\n\r\n") == 0);
    buf_free(&b);
}

static void writes_error_page(void) {
    buf b = BUF_INIT;
    CHECK_INT(http_write_error(&b, 404, 0, 0), 0);
    buf_append(&b, "", 1);
    const char *s = buf_ptr(&b);
    CHECK(strncmp(s, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK_HAS(s, "Connection: close\r\n");
    const char *body = strstr(s, "\r\n\r\n") + 4;
    const char *cl = strstr(s, "Content-Length: ") + 16;
    CHECK_INT(atoi(cl), strlen(body));
    CHECK_HAS(body, "404 Not Found");
    buf_free(&b);

    /* HEAD: 길이는 같고 본문은 없음 */
    CHECK_INT(http_write_error(&b, 404, 0, 1), 0);
    buf_append(&b, "", 1);
    s = buf_ptr(&b);
    CHECK(strcmp(s + strlen(s) - 4, "\r\n\r\n") == 0);
    CHECK(atoi(strstr(s, "Content-Length: ") + 16) > 0);
    buf_free(&b);
}

static void method_names(void) {
    CHECK_INT(http_method_from_str("GET"), HTTP_M_GET);
    CHECK_INT(http_method_from_str("PATCH"), HTTP_M_PATCH);
    CHECK_INT(http_method_from_str("CONNECT"), -1);
}

int main(void) {
    RUN(simple_get);
    RUN(incomplete_until_blank_line);
    RUN(head_len_excludes_body_and_next_request);
    RUN(headers_are_case_insensitive_and_trimmed);
    RUN(keep_alive_rules);
    RUN(multiple_connection_headers_are_combined);
    RUN(request_line_limit_is_independent_of_chunking);
    RUN(tokens);
    RUN(accepts_absolute_form_and_leading_blank_lines);
    RUN(duplicate_equal_content_length_is_ok);
    RUN(malformed_requests);
    RUN(nul_byte_in_head_is_rejected);
    RUN(unsupported_things);
    RUN(future_minor_version_is_treated_as_1_1);
    RUN(size_limits);
    RUN(response_head);
    RUN(status_text);
    RUN(writes_head);
    RUN(writes_error_page);
    RUN(method_names);
    TEST_DONE();
}
