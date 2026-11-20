#include "event.h"

#include <errno.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <unistd.h>

#define MAX_BATCH 256

int ev_create(void) { return epoll_create1(EPOLL_CLOEXEC); }

static uint32_t to_epoll(unsigned ev) {
    uint32_t r = 0;
    if (ev & EV_READ) r |= EPOLLIN;
    if (ev & EV_WRITE) r |= EPOLLOUT;
    return r;
}

int ev_add(int ep, int fd, unsigned events, void *ptr) {
    struct epoll_event e = {0};
    e.events = to_epoll(events);
    e.data.ptr = ptr;
    return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
}

int ev_mod(int ep, int fd, unsigned events, void *ptr) {
    struct epoll_event e = {0};
    e.events = to_epoll(events);
    e.data.ptr = ptr;
    return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &e);
}

int ev_del(int ep, int fd) { return epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL); }

int ev_wait(int ep, ev_event *out, int max, int timeout_ms) {
    struct epoll_event evs[MAX_BATCH];
    if (max > MAX_BATCH) max = MAX_BATCH;
    int n = epoll_wait(ep, evs, max, timeout_ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) {
        out[i].ptr = evs[i].data.ptr;
        out[i].events = 0;
        if (evs[i].events & EPOLLIN) out[i].events |= EV_READ;
        if (evs[i].events & EPOLLOUT) out[i].events |= EV_WRITE;
        if (evs[i].events & (EPOLLHUP | EPOLLERR)) out[i].events |= EV_ERR;
    }
    return n;
}
