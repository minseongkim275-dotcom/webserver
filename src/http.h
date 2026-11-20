/* http: 요청 머리 파싱, 응답 머리 파싱, 응답 머리 만들기.
 * 소켓을 만지지 않는다. 지금까지 받은 바이트를 넘기면 "더 필요함 / 완성 / 오류" 중 하나를 돌려준다. */
#ifndef WS_HTTP_H
#define WS_HTTP_H

#include <stddef.h>

#include "buf.h"

#define HTTP_MAX_REQUEST_LINE 8192  /* 넘으면 414 */
#define HTTP_MAX_HEAD 16384         /* 머리 전체(요청 라인 + 헤더). 넘으면 431 */
#define HTTP_MAX_HEADERS 64         /* 넘으면 431 */
#define HTTP_MAX_CONTENT_LENGTH 1000000000000000LL /* 이보다 큰 숫자는 형식 오류로 본다 */

typedef enum {
    HTTP_M_GET,
    HTTP_M_HEAD,
    HTTP_M_POST,
    HTTP_M_PUT,
    HTTP_M_DELETE,
    HTTP_M_OPTIONS,
    HTTP_M_PATCH,
} http_method;

typedef enum {
    HTTP_PARSE_ERROR = -1,
    HTTP_PARSE_INCOMPLETE = 0, /* 머리 끝(빈 줄)을 아직 못 받음 */
    HTTP_PARSE_OK = 1,
} http_parse_status;

typedef struct {
    const char *name;
    const char *value; /* 앞뒤 공백을 뺀 값 */
} http_header;

typedef struct {
    char *raw;        /* 머리 복사본. 아래 문자열들이 이 안을 가리킨다 */
    char *pathbuf;    /* path와 query가 가리키는 복사본 */
    size_t head_len;  /* 받은 데이터 중 머리가 차지한 길이 (빈 줄 포함). 본문은 그 뒤부터 */
    const char *method_str;
    http_method method;
    const char *target; /* 받은 그대로의 요청 대상 */
    const char *path;   /* '?' 앞부분. absolute-form이면 스킴과 호스트를 뗀 경로. 디코딩 전 */
    const char *query;  /* '?' 뒷부분, 없으면 NULL */
    int version;        /* 10 = HTTP/1.0, 11 = HTTP/1.1 */
    http_header headers[HTTP_MAX_HEADERS];
    size_t nheaders;
    long long content_length; /* 없으면 -1 */
    int keep_alive;
    const char *host; /* 없으면 NULL (HTTP/1.0만 가능) */
} http_request;

/* data[0..len)이 지금까지 받은 바이트다.
 * OK이면 req를 채운다 (http_request_free로 정리). ERROR이면 *err_status에 응답할 상태 코드를 넣는다. */
http_parse_status http_parse_request(const char *data, size_t len, http_request *req, int *err_status);
void http_request_free(http_request *req);

/* 이름은 대소문자를 가리지 않는다. 같은 이름이 여럿이면 첫 번째 */
const char *http_header_get(const http_request *req, const char *name);
/* "a, B ,c" 같은 쉼표 목록에 token이 있는가 (대소문자 무시) */
int http_has_token(const char *list, const char *token);
/* 메서드 이름 → 값. 모르는 메서드면 -1 */
int http_method_from_str(const char *s);

/* 백엔드 응답 머리 */
typedef struct {
    int version;
    int status;
    long long content_length; /* 없으면 -1 */
    int chunked;
    size_t head_len;
} http_response_info;

http_parse_status http_parse_response_head(const char *data, size_t len, http_response_info *info);

const char *http_status_text(int status);

/* 상태 라인과 공통 헤더(Server, Date, Content-Type, Content-Length, Connection)를 쓴다.
 * extra_headers는 "Name: value\r\n" 꼴로 이미 만든 줄들(없으면 NULL). 빈 줄까지 쓴다. */
int http_write_head(buf *out, int status, const char *content_type, long long content_length,
                    int keep_alive, const char *extra_headers);
/* 작은 HTML 오류 페이지. head_only이면 본문은 빼고 Content-Length만 맞춘다 */
int http_write_error(buf *out, int status, int keep_alive, int head_only);

#endif
