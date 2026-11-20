#include "proxy.h"

#include <string.h>
#include <strings.h>

/* Connection 헤더에 이 이름이 적혀 있는가 (적힌 헤더는 홉 단위라 전달하지 않는다) */
static int named_in_connection(const http_request *req, const char *name) {
    for (size_t i = 0; i < req->nheaders; i++)
        if (strcasecmp(req->headers[i].name, "Connection") == 0 && http_has_token(req->headers[i].value, name))
            return 1;
    return 0;
}

static int is_hop_by_hop(const char *name) {
    static const char *const hop[] = {"Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer", "Upgrade", "Expect"};
    for (size_t i = 0; i < sizeof hop / sizeof hop[0]; i++)
        if (strcasecmp(name, hop[i]) == 0) return 1;
    return 0;
}

int proxy_build_request(const http_request *req, const char *client_ip, const char *fallback_host, buf *out) {
    if (buf_printf(out, "%s %s%s%s HTTP/1.1\r\n", req->method_str, req->path, req->query ? "?" : "",
                   req->query ? req->query : "") < 0)
        return -1;

    buf xff = BUF_INIT;
    int has_host = 0;
    for (size_t i = 0; i < req->nheaders; i++) {
        const char *name = req->headers[i].name, *value = req->headers[i].value;
        if (strcasecmp(name, "X-Forwarded-For") == 0) {
            if (value[0] && ((buf_size(&xff) && buf_append_str(&xff, ", ") < 0) || buf_append_str(&xff, value) < 0)) {
                buf_free(&xff);
                return -1;
            }
            continue;
        }
        if (strcasecmp(name, "X-Forwarded-Proto") == 0) continue;
        if (is_hop_by_hop(name) || named_in_connection(req, name)) continue;
        if (strcasecmp(name, "Host") == 0) has_host = 1;
        if (buf_printf(out, "%s: %s\r\n", name, value) < 0) {
            buf_free(&xff);
            return -1;
        }
    }
    int bad = 0;
    if (!has_host && buf_printf(out, "Host: %s\r\n", fallback_host) < 0) bad = 1;
    if (!bad && buf_size(&xff) && buf_append(&xff, ", ", 2) < 0) bad = 1;
    if (!bad && buf_append_str(&xff, client_ip) < 0) bad = 1;
    if (!bad && buf_append(&xff, "", 1) < 0) bad = 1; /* NUL로 끝내 %s로 쓴다 */
    if (!bad && buf_printf(out, "X-Forwarded-For: %s\r\nX-Forwarded-Proto: http\r\nConnection: close\r\n\r\n",
                           buf_ptr(&xff)) < 0)
        bad = 1;
    buf_free(&xff);
    return bad ? -1 : 0;
}

int proxy_rewrite_response_head(const char *head, size_t len, const http_response_info *info, int keep_alive, buf *out) {
    const char *p = head, *end = head + len;
    /* 상태 줄은 그대로 */
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    if (!eol) return -1;
    if (buf_append(out, p, (size_t)(eol + 1 - p)) < 0) return -1;
    p = eol + 1;

    while (p < end) {
        eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) break;
        size_t line_len = (size_t)(eol + 1 - p);
        if (line_len <= 2) break; /* 빈 줄 */
        const char *colon = memchr(p, ':', line_len);
        size_t nl = colon ? (size_t)(colon - p) : 0;
        int drop = 0;
        if (colon) {
            drop = (nl == 10 && strncasecmp(p, "Connection", 10) == 0) || (nl == 10 && strncasecmp(p, "Keep-Alive", 10) == 0) ||
                   (nl == 16 && strncasecmp(p, "Proxy-Connection", 16) == 0) ||
                   (info->chunked && nl == 14 && strncasecmp(p, "Content-Length", 14) == 0);
        }
        if (!drop && buf_append(out, p, line_len) < 0) return -1;
        p = eol + 1;
    }
    if (buf_printf(out, "Connection: %s\r\n\r\n", keep_alive ? "keep-alive" : "close") < 0) return -1;
    return 0;
}

int proxy_response_has_no_body(int request_method, int status) {
    return request_method == HTTP_M_HEAD || (status >= 100 && status < 200) || status == 204 || status == 304;
}
