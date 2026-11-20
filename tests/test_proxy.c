#include "proxy.h"
#include "test.h"

#include <stdlib.h>

static char *build(const char *raw, const char *ip, const char *fallback) {
    http_request req;
    int err;
    if (http_parse_request(raw, strlen(raw), &req, &err) != HTTP_PARSE_OK) return NULL;
    buf b = BUF_INIT;
    int rc = proxy_build_request(&req, ip, fallback, &b);
    http_request_free(&req);
    if (rc < 0 || buf_append(&b, "", 1) < 0) {
        buf_free(&b);
        return NULL;
    }
    char *s = strdup(buf_ptr(&b));
    buf_free(&b);
    return s;
}

static void builds_request(void) {
    char *s = build("GET /api/hello?x=1 HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n",
                    "10.0.0.7", "127.0.0.1:9001");
    CHECK(s != NULL);
    CHECK(strncmp(s, "GET /api/hello?x=1 HTTP/1.1\r\n", 29) == 0);
    CHECK_HAS(s, "\r\nHost: example.com\r\n");
    CHECK_HAS(s, "\r\nAccept: */*\r\n");
    CHECK_HAS(s, "\r\nX-Forwarded-For: 10.0.0.7\r\n");
    CHECK_HAS(s, "\r\nX-Forwarded-Proto: http\r\n");
    CHECK_HAS(s, "\r\nConnection: close\r\n");
    CHECK(strstr(s, "keep-alive") == NULL);
    CHECK(strcmp(s + strlen(s) - 4, "\r\n\r\n") == 0);
    free(s);
}

static void drops_hop_by_hop_headers(void) {
    char *s = build("POST /api/x HTTP/1.1\r\nHost: h\r\nKeep-Alive: timeout=5\r\nProxy-Connection: keep-alive\r\n"
                    "TE: trailers\r\nTrailer: X\r\nUpgrade: websocket\r\nExpect: 100-continue\r\n"
                    "Connection: X-Secret, close\r\nX-Secret: s\r\nX-Keep: k\r\nContent-Length: 3\r\n\r\n",
                    "1.2.3.4", "b:1");
    CHECK(s != NULL);
    CHECK(strstr(s, "Keep-Alive") == NULL);
    CHECK(strstr(s, "Proxy-Connection") == NULL);
    CHECK(strstr(s, "TE:") == NULL);
    CHECK(strstr(s, "Trailer") == NULL);
    CHECK(strstr(s, "Upgrade") == NULL);
    CHECK(strstr(s, "Expect") == NULL);
    CHECK(strstr(s, "X-Secret") == NULL); /* Connection에 이름이 적힌 헤더 */
    CHECK_HAS(s, "\r\nX-Keep: k\r\n");
    CHECK_HAS(s, "\r\nContent-Length: 3\r\n");
    free(s);
}

static void appends_to_existing_x_forwarded_for(void) {
    char *s = build("GET / HTTP/1.1\r\nHost: h\r\nX-Forwarded-For: 9.9.9.9\r\nX-Forwarded-For: 8.8.8.8\r\n"
                    "X-Forwarded-Proto: https\r\n\r\n",
                    "1.2.3.4", "b:1");
    CHECK(s != NULL);
    CHECK_HAS(s, "\r\nX-Forwarded-For: 9.9.9.9, 8.8.8.8, 1.2.3.4\r\n");
    CHECK(strstr(s, "X-Forwarded-Proto: https") == NULL); /* 우리가 직접 받은 것은 http다 */
    CHECK_HAS(s, "X-Forwarded-Proto: http\r\n");
    free(s);
}

static void host_fallback_and_absolute_form(void) {
    char *s = build("GET / HTTP/1.0\r\n\r\n", "1.2.3.4", "127.0.0.1:9001");
    CHECK(s != NULL);
    CHECK_HAS(s, "\r\nHost: 127.0.0.1:9001\r\n");
    CHECK(strncmp(s, "GET / HTTP/1.1\r\n", 16) == 0); /* 백엔드에는 항상 1.1로 보낸다 */
    free(s);

    s = build("GET http://example.com/a/b?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n", "1.2.3.4", "b:1");
    CHECK(s != NULL);
    CHECK(strncmp(s, "GET /a/b?q=1 HTTP/1.1\r\n", 23) == 0); /* origin-form으로 바뀐다 */
    free(s);
}

static char *rewrite(const char *head, int keep_alive) {
    http_response_info info;
    CHECK_INT(http_parse_response_head(head, strlen(head), &info), HTTP_PARSE_OK);
    buf b = BUF_INIT;
    proxy_rewrite_response_head(head, info.head_len, &info, keep_alive, &b);
    buf_append(&b, "", 1);
    char *s = strdup(buf_ptr(&b));
    buf_free(&b);
    return s;
}

static void rewrites_response_head(void) {
    char *s = rewrite("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n"
                      "Keep-Alive: timeout=1\r\nX-Backend: a\r\n\r\n",
                      1);
    CHECK(strncmp(s, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK_HAS(s, "\r\nContent-Type: text/plain\r\n");
    CHECK_HAS(s, "\r\nContent-Length: 5\r\n");
    CHECK_HAS(s, "\r\nX-Backend: a\r\n");
    CHECK_HAS(s, "\r\nConnection: keep-alive\r\n");
    CHECK(strstr(s, "Connection: close") == NULL);
    CHECK(strstr(s, "Keep-Alive:") == NULL);
    CHECK(strcmp(s + strlen(s) - 4, "\r\n\r\n") == 0);
    free(s);

    s = rewrite("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n", 0);
    CHECK(strncmp(s, "HTTP/1.0 404 Not Found\r\n", 24) == 0); /* 상태 줄은 그대로 */
    CHECK_HAS(s, "\r\nConnection: close\r\n");
    free(s);
}

static void chunked_wins_over_content_length(void) {
    char *s = rewrite("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 99\r\n\r\n", 0);
    CHECK_HAS(s, "\r\nTransfer-Encoding: chunked\r\n");
    CHECK(strstr(s, "Content-Length") == NULL);
    free(s);
}

static void bodyless_responses(void) {
    CHECK_INT(proxy_response_has_no_body(HTTP_M_HEAD, 200), 1);
    CHECK_INT(proxy_response_has_no_body(HTTP_M_GET, 204), 1);
    CHECK_INT(proxy_response_has_no_body(HTTP_M_GET, 304), 1);
    CHECK_INT(proxy_response_has_no_body(HTTP_M_GET, 100), 1);
    CHECK_INT(proxy_response_has_no_body(HTTP_M_GET, 200), 0);
    CHECK_INT(proxy_response_has_no_body(HTTP_M_POST, 404), 0);
}

int main(void) {
    RUN(builds_request);
    RUN(drops_hop_by_hop_headers);
    RUN(appends_to_existing_x_forwarded_for);
    RUN(host_fallback_and_absolute_form);
    RUN(rewrites_response_head);
    RUN(chunked_wins_over_content_length);
    RUN(bodyless_responses);
    TEST_DONE();
}
