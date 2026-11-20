/* server: epoll 이벤트 루프와 연결 상태 머신. 소켓과 epoll을 만지는 유일한 모듈이다. */
#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <signal.h>

#include "config.h"

/* root_real은 realpath로 정규화한 정적 파일 root. *stop이 0이 아니게 되면 안전하게 종료한다.
 * 정상 종료는 0, 시작 실패(bind 실패 등)는 1을 돌려준다. */
int server_run(const config *cfg, const char *root_real, volatile sig_atomic_t *stop);

#endif
