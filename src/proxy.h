/* proxy: 백엔드로 보낼 요청 머리를 만들고, 백엔드 응답 머리를 클라이언트용으로 고쳐 쓴다. 소켓은 만지지 않는다. */
#ifndef WS_PROXY_H
#define WS_PROXY_H

#include "buf.h"
#include "http.h"

/* "METHOD path[?query] HTTP/1.1" + 헤더 + 빈 줄을 out에 쓴다 (본문은 호출한 쪽이 뒤에 붙인다).
 * - 홉 단위 헤더(Connection, Keep-Alive, Proxy-Connection, TE, Trailer, Upgrade, Expect)와
 *   Connection에 이름이 적힌 헤더를 뺀다.
 * - X-Forwarded-For는 기존 값 뒤에 client_ip를 덧붙이고, X-Forwarded-Proto: http를 넣는다.
 * - Host가 없으면 fallback_host(백엔드 주소)를 넣는다. 마지막에 Connection: close.
 * 메모리가 모자라면 -1 */
int proxy_build_request(const http_request *req, const char *client_ip, const char *fallback_host, buf *out);

/* 백엔드 응답 머리(head[0..len), 빈 줄 포함)를 클라이언트용으로 고쳐 쓴다.
 * Connection/Keep-Alive/Proxy-Connection을 빼고 Connection: keep-alive|close를 넣는다.
 * info->chunked이면 Content-Length를 뺀다 (둘이 같이 오면 chunked가 이긴다). */
int proxy_rewrite_response_head(const char *head, size_t len, const http_response_info *info, int keep_alive, buf *out);

/* 응답에 본문이 없는가: HEAD 요청에 대한 응답, 1xx, 204, 304 */
int proxy_response_has_no_body(int request_method, int status);

#endif
