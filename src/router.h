/* router: 경로 → upstream 선택, upstream 안에서 백엔드 선택(Round-Robin). 소켓은 만지지 않는다. */
#ifndef WS_ROUTER_H
#define WS_ROUTER_H

#include <time.h>

#include "config.h"

#define ROUTER_DOWN_SECS 10 /* 연결에 실패한 백엔드를 이 시간 동안 건너뛴다 */

/* 가장 긴 접두사가 맞는 route의 upstream 번호. 정적 파일이면(route가 static이거나 맞는 route가 없으면) -1.
 * 접두사 경계: 접두사가 '/'로 끝나거나, path가 접두사와 같거나, 접두사 다음 글자가 '/'.
 * (/api는 /api, /api/x에 맞고 /apix에는 맞지 않는다.) */
int router_match(const config *c, const char *path);

/* upstream 안에서 다음 백엔드 번호. down 표시된 백엔드는 건너뛰고, 전부 down이면 그냥 다음 차례 */
int router_pick(upstream *u, time_t now);
void router_mark_down(upstream *u, int backend_index, time_t now);

#endif
