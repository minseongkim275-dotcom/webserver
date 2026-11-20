/* event: epoll을 감싼 아주 얇은 층. 이벤트 플래그를 자체 상수로 두어 server.c가 epoll 헤더를 직접 쓰지 않게 한다.
 * (macOS용 kqueue/poll 구현을 추가할 때 이 파일만 바꾸면 되도록 한 경계다. 지금 구현은 epoll 하나뿐이다.) */
#ifndef WS_EVENT_H
#define WS_EVENT_H

#define EV_READ 1u
#define EV_WRITE 2u
#define EV_ERR 4u /* HUP 또는 ERR. 항상 보고된다 */

typedef struct {
    void *ptr;
    unsigned events;
} ev_event;

int ev_create(void); /* 실패하면 -1 */
int ev_add(int ep, int fd, unsigned events, void *ptr);
int ev_mod(int ep, int fd, unsigned events, void *ptr);
int ev_del(int ep, int fd);
/* 준비된 이벤트 수를 돌려준다. 시그널로 깨어나면 0 (errno는 건드리지 않고 정상으로 본다), 오류면 -1 */
int ev_wait(int ep, ev_event *out, int max, int timeout_ms);

#endif
