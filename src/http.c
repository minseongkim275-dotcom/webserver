#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define SERVER_NAME "jungle-webserver"

static const char *const method_names[] = {
    [HTTP_M_GET] = "GET",         [HTTP_M_HEAD] = "HEAD",   [HTTP_M_POST] = "POST",
    [HTTP_M_PUT] = "PUT",         [HTTP_M_DELETE] = "DELETE", [HTTP_M_OPTIONS] = "OPTIONS",
    [HTTP_M_PATCH] = "PATCH",
};

int http_method_from_str(const char *s) {
    for (size_t i = 0; i < sizeof method_names / sizeof method_names[0]; i++)
        if (strcmp(s, method_names[i]) == 0) return (int)i;
    return -1;
}

/* RFC 7230 tchar: 메서드와 헤더 이름에 쓸 수 있는 문자. locale에 따라 달라지지 않게 직접 비교한다. */
static int is_tchar(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int all_tchar(const char *s) {
    if (*s == '\0') return 0;
    for (; *s; s++)
        if (!is_tchar((unsigned char)*s)) return 0;
    return 1;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static const char *find_seq(const char *p, size_t n, const char *seq, size_t sn) {
    if (n < sn) return NULL;
    for (size_t i = 0; i + sn <= n; i++)
        if (p[i] == seq[0] && memcmp(p + i, seq, sn) == 0) return p + i;
    return NULL;
}

/* 숫자만으로 된 Content-Length를 읽는다. 형식이 틀리거나 너무 크면 -1 */
static long long parse_length(const char *s, size_t n) {
    if (n == 0) return -1;
    long long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i])) return -1;
        v = v * 10 + (s[i] - '0');
        if (v > HTTP_MAX_CONTENT_LENGTH) return -1;
    }
    return v;
}

static int has_token_n(const char *list, size_t n, const char *token) {
    size_t tl = strlen(token);
    size_t i = 0;
    while (i < n) {
        while (i < n && (list[i] == ' ' || list[i] == '\t' || list[i] == ',')) i++;
        size_t s = i;
        while (i < n && list[i] != ',') i++;
        size_t e = i;
        while (e > s && (list[e - 1] == ' ' || list[e - 1] == '\t')) e--;
        if (e - s == tl && strncasecmp(list + s, token, tl) == 0) return 1;
    }
    return 0;
}

int http_has_token(const char *list, const char *token) {
    return list ? has_token_n(list, strlen(list), token) : 0;
}

const char *http_header_get(const http_request *req, const char *name) {
    for (size_t i = 0; i < req->nheaders; i++)
        if (strcasecmp(req->headers[i].name, name) == 0) return req->headers[i].value;
    return NULL;
}

void http_request_free(http_request *req) {
    free(req->raw);
    free(req->pathbuf);
    req->raw = req->pathbuf = NULL;
}

/* 줄 안에 홀로 쓴 CR/LF나 제어 문자가 있는가 (HT는 허용) */
static int has_bad_ctl(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && c != '\t') || c == 0x7f) return 1;
    }
    return 0;
}

/* 요청 대상을 path와 query로 나눈다. 실패하면 -1 */
static int split_target(http_request *req) {
    req->pathbuf = strdup(req->target);
    if (!req->pathbuf) return -1;
    char *p = req->pathbuf;

    if (p[0] != '/') {
        /* absolute-form: http://host[:port][/path][?query] */
        if (strncasecmp(p, "http://", 7) != 0) return -1;
        char *rest = strpbrk(p + 7, "/?");
        if (!rest || *rest == '?') {
            req->path = "/";
            req->query = rest ? rest + 1 : NULL;
            return 0;
        }
        p = rest;
    }
    char *q = strchr(p, '?');
    if (q) {
        *q = '\0';
        req->query = q + 1;
    }
    req->path = p;
    return 0;
}

/* 머리 하나가 들어 있는 raw(NUL로 끝남)를 조각낸다. 성공하면 0, 실패하면 응답할 상태 코드 */
static int parse_head(http_request *req) {
    char *line = req->raw;
    char *eol = strstr(line, "\r\n");
    *eol = '\0';
    if (has_bad_ctl(line)) return 400;

    /* 요청 라인: METHOD SP target SP HTTP/x.y (공백은 정확히 하나씩) */
    char *sp1 = strchr(line, ' ');
    if (!sp1) return 400;
    *sp1 = '\0';
    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2) return 400;
    *sp2 = '\0';
    char *version = sp2 + 1;
    if (!all_tchar(line) || *target == '\0' || strchr(version, ' ')) return 400;
    for (const char *t = target; *t; t++)
        if ((unsigned char)*t <= 0x20 || (unsigned char)*t >= 0x7f) return 400;
    if (strlen(version) != 8 || strncmp(version, "HTTP/", 5) != 0 || !is_digit(version[5]) ||
        version[6] != '.' || !is_digit(version[7]))
        return 400;

    req->method_str = line;
    req->target = target;
    if (version[5] != '1') return 505;
    req->version = version[7] == '0' ? 10 : 11; /* 1.2 이상은 1.1로 다룬다 (RFC 7230 2.6) */
    int m = http_method_from_str(line);
    if (m < 0) return 501;
    req->method = (http_method)m;
    if (split_target(req) < 0) return req->pathbuf ? 400 : 500;

    /* 헤더 줄들 */
    int host_count = 0, has_te = 0;
    char *p = eol + 2;
    while (!(p[0] == '\r' && p[1] == '\n')) {
        eol = strstr(p, "\r\n");
        *eol = '\0';
        if (p[0] == ' ' || p[0] == '\t') return 400; /* obs-fold */
        if (has_bad_ctl(p)) return 400;
        char *colon = strchr(p, ':');
        if (!colon) return 400;
        *colon = '\0';
        if (!all_tchar(p)) return 400; /* 이름이 비었거나 콜론 앞에 공백 */
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = '\0';

        if (req->nheaders == HTTP_MAX_HEADERS) return 431;
        req->headers[req->nheaders].name = p;
        req->headers[req->nheaders].value = v;
        req->nheaders++;

        if (strcasecmp(p, "Host") == 0) {
            host_count++;
            req->host = v;
        } else if (strcasecmp(p, "Content-Length") == 0) {
            long long n = parse_length(v, strlen(v));
            if (n < 0) return 400;
            if (req->content_length >= 0 && req->content_length != n) return 400;
            req->content_length = n;
        } else if (strcasecmp(p, "Transfer-Encoding") == 0) {
            has_te = 1;
        }
        p = eol + 2;
    }

    if (host_count > 1 || (req->version == 11 && host_count == 0)) return 400;
    if (has_te) return 501; /* chunked 요청 본문은 지원하지 않는다 */

    /* 같은 이름의 헤더가 여럿이면 쉼표로 이은 목록과 같다 (RFC 7230 3.2.2): 모든 Connection 줄을 본다 */
    int has_close = 0, has_keep = 0;
    for (size_t i = 0; i < req->nheaders; i++) {
        if (strcasecmp(req->headers[i].name, "Connection") != 0) continue;
        if (http_has_token(req->headers[i].value, "close")) has_close = 1;
        if (http_has_token(req->headers[i].value, "keep-alive")) has_keep = 1;
    }
    req->keep_alive = req->version == 11 ? !has_close : (has_keep && !has_close);
    return 0;
}

http_parse_status http_parse_request(const char *data, size_t len, http_request *req, int *err_status) {
    memset(req, 0, sizeof *req);
    req->content_length = -1;

    /* 요청 라인 앞의 빈 줄은 건너뛴다 (RFC 7230 3.5). 끝없이 보내면 400 */
    size_t start = 0;
    while (start + 2 <= len && data[start] == '\r' && data[start + 1] == '\n') start += 2;
    if (start > HTTP_MAX_HEAD) {
        *err_status = 400;
        return HTTP_PARSE_ERROR;
    }
    const char *p = data + start;
    size_t avail = len - start;

    const char *line_end = find_seq(p, avail, "\r\n", 2);
    /* CRLF가 아직 없으면 끝에 걸린 '\r' 한 바이트는 줄 길이에 넣지 않는다 (끊겨 도착해도 같은 결과) */
    size_t line_len = line_end ? (size_t)(line_end - p) : avail - (avail > 0 && p[avail - 1] == '\r' ? 1 : 0);
    if (line_len > HTTP_MAX_REQUEST_LINE) {
        *err_status = 414;
        return HTTP_PARSE_ERROR;
    }
    const char *end = find_seq(p, avail, "\r\n\r\n", 4);
    if (!end) {
        if (avail > HTTP_MAX_HEAD) {
            *err_status = 431;
            return HTTP_PARSE_ERROR;
        }
        return HTTP_PARSE_INCOMPLETE;
    }
    size_t n = (size_t)(end - p) + 4;
    if (n > HTTP_MAX_HEAD) {
        *err_status = 431;
        return HTTP_PARSE_ERROR;
    }
    if (memchr(p, '\0', n)) {
        *err_status = 400;
        return HTTP_PARSE_ERROR;
    }

    req->raw = malloc(n + 1);
    if (!req->raw) {
        *err_status = 500;
        return HTTP_PARSE_ERROR;
    }
    memcpy(req->raw, p, n);
    req->raw[n] = '\0';
    req->head_len = start + n;

    int st = parse_head(req);
    if (st != 0) {
        http_request_free(req);
        *err_status = st;
        return HTTP_PARSE_ERROR;
    }
    return HTTP_PARSE_OK;
}

http_parse_status http_parse_response_head(const char *data, size_t len, http_response_info *info) {
    memset(info, 0, sizeof *info);
    info->content_length = -1;

    const char *end = find_seq(data, len, "\r\n\r\n", 4);
    if (!end) return len > HTTP_MAX_HEAD ? HTTP_PARSE_ERROR : HTTP_PARSE_INCOMPLETE;
    size_t n = (size_t)(end - data) + 4;
    if (n > HTTP_MAX_HEAD) return HTTP_PARSE_ERROR;

    /* 상태 라인: HTTP/1.x SP 3DIGIT [SP reason] */
    if (n < 14 || strncmp(data, "HTTP/1.", 7) != 0 || !is_digit(data[7]) || data[8] != ' ' ||
        !is_digit(data[9]) || !is_digit(data[10]) || !is_digit(data[11]) || (data[12] != ' ' && data[12] != '\r'))
        return HTTP_PARSE_ERROR;
    info->version = data[7] == '0' ? 10 : 11;
    info->status = (data[9] - '0') * 100 + (data[10] - '0') * 10 + (data[11] - '0');
    if (info->status < 100) return HTTP_PARSE_ERROR;
    info->head_len = n;

    const char *line = find_seq(data, n, "\r\n", 2) + 2;
    const char *stop = data + n - 2; /* 마지막 빈 줄 */
    while (line < stop) {
        const char *eol = find_seq(line, (size_t)(stop + 2 - line), "\r\n", 2);
        const char *colon = memchr(line, ':', (size_t)(eol - line));
        if (!colon || colon == line) return HTTP_PARSE_ERROR;
        size_t nl = (size_t)(colon - line);
        const char *v = colon + 1, *ve = eol;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;

        if (nl == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
            long long cl = parse_length(v, (size_t)(ve - v));
            if (cl < 0 || (info->content_length >= 0 && info->content_length != cl)) return HTTP_PARSE_ERROR;
            info->content_length = cl;
        } else if (nl == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0) {
            if (has_token_n(v, (size_t)(ve - v), "chunked")) info->chunked = 1;
        }
        line = eol + 2;
    }
    return HTTP_PARSE_OK;
}

const char *http_status_text(int status) {
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

int http_write_head(buf *out, int status, const char *content_type, long long content_length,
                    int keep_alive, const char *extra_headers) {
    char date[64];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);

    if (buf_printf(out, "HTTP/1.1 %d %s\r\nServer: " SERVER_NAME "\r\nDate: %s\r\n", status,
                   http_status_text(status), date) < 0)
        return -1;
    if (content_type && buf_printf(out, "Content-Type: %s\r\n", content_type) < 0) return -1;
    if (content_length >= 0 && buf_printf(out, "Content-Length: %lld\r\n", content_length) < 0) return -1;
    if (buf_printf(out, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close") < 0) return -1;
    if (extra_headers && buf_append_str(out, extra_headers) < 0) return -1;
    return buf_append(out, "\r\n", 2);
}

int http_write_error(buf *out, int status, int keep_alive, int head_only) {
    char body[512];
    const char *text = http_status_text(status);
    int n = snprintf(body, sizeof body,
                     "<!DOCTYPE html>\n<html><head><title>%d %s</title></head>\n"
                     "<body><h1>%d %s</h1><hr><p>" SERVER_NAME "</p></body></html>\n",
                     status, text, status, text);
    if (n < 0 || (size_t)n >= sizeof body) return -1;
    if (http_write_head(out, status, "text/html; charset=utf-8", n, keep_alive, NULL) < 0) return -1;
    return head_only ? 0 : buf_append(out, body, (size_t)n);
}
