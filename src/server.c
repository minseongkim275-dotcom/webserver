#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "event.h"
#include "fsutil.h"
#include "http.h"
#include "mime.h"
#include "proxy.h"
#include "router.h"

#define PROXY_HIGH (256 * 1024) /* 클라이언트 쪽 출력이 이만큼 쌓이면 백엔드에서 읽기를 멈춘다 */
#define READ_CHUNK 16384
#define FILE_CHUNK 65536
#define MAX_EVENTS 256
#define LINGER_SECS 2    /* 오류 응답 뒤 클라이언트가 보내는 나머지를 버리며 기다리는 시간 */
#define SHUTDOWN_GRACE 5 /* 종료 신호 뒤 진행 중인 응답을 기다리는 최대 시간(초) */
#define ACCEPT_PAUSE 1   /* fd가 모자라거나 연결이 한도에 닿았을 때 accept를 쉬는 시간(초) */

typedef enum { K_LISTEN, K_CLIENT, K_UPSTREAM } kind_t;
/* S_PROXY: 요청을 백엔드에 넘기고 응답을 기다리거나 중계하는 중 (클라이언트 연결의 상태) */
typedef enum { S_READ_HEAD, S_READ_BODY, S_WRITE, S_LINGER, S_PROXY } state_t;
/* 백엔드 연결(K_UPSTREAM)의 상태 */
typedef enum { U_CONNECT, U_SEND, U_HEAD, U_RELAY } ustate_t;

typedef struct conn {
    int fd;
    kind_t kind;
    state_t state;
    unsigned interest; /* 지금 epoll에 등록된 관심사 */
    buf in, out;
    http_request req;
    int have_req;
    int sent_continue; /* 100 Continue를 이미 보냈는가 */
    int keep_alive;    /* 지금 응답 뒤에 연결을 유지하는가 */
    int linger;        /* 응답 뒤 바로 닫지 않고 나머지 입력을 버리며 기다리는가 */
    int status;        /* 지금 응답의 상태 코드 (로그용) */
    long long bytes_out;
    int file_fd; /* 보낼 파일. 없으면 -1 */
    off_t file_off, file_end;
    time_t last_active;     /* 마지막으로 읽거나 쓴 시각 (monotonic 초) */
    time_t linger_deadline;
    struct timespec t0;     /* 요청 처리를 시작한 시각 (로그용) */
    char ip[INET_ADDRSTRLEN];
    /* 프록시: 클라이언트 연결과 백엔드 연결은 서로를 peer로 가리킨다 */
    struct conn *peer;
    ustate_t ustate;           /* K_UPSTREAM만 */
    long long relay_remaining; /* K_UPSTREAM: 더 중계할 본문 길이. -1이면 백엔드가 닫을 때까지 */
    int up_idx;                /* K_CLIENT: 쓰고 있는 upstream 번호 */
    int attempts;              /* K_CLIENT: 지금까지 연결을 시도한 백엔드 수 */
    int resp_started;          /* K_CLIENT: 응답 머리를 클라이언트 출력에 쓰기 시작했는가 */
    const char *backend_label; /* K_CLIENT: 로그에 남길 백엔드 주소 (config 안을 가리킴) */
    struct conn *prev, *next;  /* 살아 있는 연결 목록 */
    struct conn *dead_next;   /* 지연 해제 목록 */
    int closed;
} conn;

typedef struct {
    config *cfg; /* Round-Robin 커서와 백엔드 down 표시를 바꾸므로 const가 아니다 */
    const char *root;
    int ep;
    int lfd;
    conn lconn; /* listen 소켓용 (kind=K_LISTEN) */
    conn *head;
    int nconns;
    conn *dead; /* 닫았지만 이번 이벤트 묶음이 끝날 때까지 해제하지 않은 연결 */
    int paused;
    time_t pause_until;
    int shutting;
    time_t deadline;
    time_t last_sweep;
} server;

static time_t mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static long ms_since(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)((now.tv_sec - t0->tv_sec) * 1000 + (now.tv_nsec - t0->tv_nsec) / 1000000);
}

/* ── 연결 수명 ── */

static void set_interest(server *s, conn *c) {
    unsigned want = 0;
    if (c->kind == K_UPSTREAM) {
        if (c->ustate == U_CONNECT || c->ustate == U_SEND) want = EV_WRITE;
        else if (c->ustate == U_HEAD) want = EV_READ;
        /* 클라이언트가 느려 출력이 쌓였으면 백엔드에서 읽지 않는다 (메모리가 무한히 늘지 않게) */
        else if (!c->peer || buf_size(&c->peer->out) < PROXY_HIGH) want = EV_READ;
    } else {
        if (c->state == S_WRITE || buf_size(&c->out) > 0) want |= EV_WRITE;
        /* S_PROXY: 요청 본문은 이미 다 받았으므로 클라이언트에서는 읽지 않는다 */
        if (c->state != S_WRITE && c->state != S_PROXY) want |= EV_READ;
    }
    if (want != c->interest) {
        /* 클라이언트가 느려서 멈춰 뒀던 백엔드 읽기를 다시 켜는 순간: 멈춘 동안 시계를 쓰지 않았으므로
         * 지금부터 다시 센다. (안 그러면 같은 반복의 sweep이 "백엔드가 오래 조용했다"며 끊는다) */
        if (c->kind == K_UPSTREAM && c->interest == 0 && want == EV_READ) c->last_active = mono();
        c->interest = want;
        if (ev_mod(s->ep, c->fd, want, c) < 0) c->interest = (unsigned)-1; /* 다음에 다시 시도 */
    }
    /* 클라이언트 출력이 줄었으면 멈춰 둔 백엔드 읽기를 다시 켠다 */
    if (c->kind == K_CLIENT && c->peer) set_interest(s, c->peer);
}

/* 닫는다. 구조체는 이번 이벤트 묶음이 끝난 뒤에 해제한다.
 * (같은 묶음 안에 이 연결의 이벤트가 더 남아 있어도 해제된 메모리를 읽지 않게 하려는 것이다.) */
static void conn_close(server *s, conn *c) {
    if (c->closed) return;
    c->closed = 1;
    conn *peer = c->peer; /* 짝 연결도 함께 닫는다 */
    c->peer = NULL;
    if (peer) {
        peer->peer = NULL;
        conn_close(s, peer);
    }
    ev_del(s->ep, c->fd);
    close(c->fd);
    if (c->file_fd >= 0) close(c->file_fd);
    if (c->have_req) http_request_free(&c->req);
    buf_free(&c->in);
    buf_free(&c->out);
    if (c->prev) c->prev->next = c->next;
    else s->head = c->next;
    if (c->next) c->next->prev = c->prev;
    s->nconns--;
    c->dead_next = s->dead;
    s->dead = c;
}

static void free_dead(server *s) {
    while (s->dead) {
        conn *c = s->dead;
        s->dead = c->dead_next;
        free(c);
    }
}

/* peer_addr가 NULL이면 백엔드 연결(처음에는 connect 완료를 기다림), 아니면 클라이언트 연결 */
static conn *conn_new(server *s, int fd, const struct sockaddr_in *peer_addr) {
    conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = fd;
    c->file_fd = -1;
    c->last_active = mono();
    if (peer_addr) {
        c->kind = K_CLIENT;
        c->state = S_READ_HEAD;
        inet_ntop(AF_INET, &peer_addr->sin_addr, c->ip, sizeof c->ip);
        c->interest = EV_READ;
    } else {
        c->kind = K_UPSTREAM;
        c->ustate = U_CONNECT;
        c->interest = EV_WRITE;
    }
    if (ev_add(s->ep, fd, c->interest, c) < 0) {
        free(c);
        return NULL;
    }
    c->next = s->head;
    if (s->head) s->head->prev = c;
    s->head = c;
    s->nconns++;
    return c;
}

/* ── 응답 만들기 ── */

static void log_access(const server *s, const conn *c) {
    if (s->cfg->quiet) return;
    if (c->have_req) {
        printf("%s \"%s %s HTTP/%d.%d\" %d %lld %ldms%s%s\n", c->ip, c->req.method_str, c->req.target,
               c->req.version / 10, c->req.version % 10, c->status, c->bytes_out, ms_since(&c->t0),
               c->backend_label ? " upstream=" : "", c->backend_label ? c->backend_label : "");
    } else {
        printf("%s \"-\" %d %lld %ldms\n", c->ip, c->status, c->bytes_out, ms_since(&c->t0));
    }
}

/* 오류 응답을 만들고 쓰기 상태로 바꾼다. 오류 응답 뒤에는 연결을 닫는다. 메모리가 모자라면 연결을 닫고 -1 */
static int respond_error(server *s, conn *c, int status, int head_only) {
    if (http_write_error(&c->out, status, 0, head_only) < 0) {
        conn_close(s, c);
        return -1;
    }
    c->status = status;
    c->keep_alive = 0;
    c->linger = 1;
    c->state = S_WRITE;
    return 0;
}

static int respond_405(server *s, conn *c) {
    static const char body[] = "405 Method Not Allowed\n";
    c->keep_alive = 0; /* 오류 응답 뒤에는 닫는다 (design.md "HTTP 규칙") */
    c->linger = 1;
    if (http_write_head(&c->out, 405, "text/plain; charset=utf-8", (long long)sizeof body - 1, c->keep_alive,
                        "Allow: GET, HEAD\r\n") < 0 ||
        buf_append(&c->out, body, sizeof body - 1) < 0) {
        conn_close(s, c);
        return -1;
    }
    c->status = 405;
    c->state = S_WRITE;
    return 0;
}

static int respond_redirect(server *s, conn *c) {
    /* 앞의 슬래시를 하나로 줄인다. "//host/dir"이 다른 사이트로 가는 주소가 되지 않게 하려는 것이다. */
    const char *p = c->req.path;
    while (p[0] == '/' && p[1] == '/') p++;
    buf loc = BUF_INIT;
    int bad = buf_printf(&loc, "Location: %s/%s%s\r\n", p, c->req.query ? "?" : "", c->req.query ? c->req.query : "") < 0;
    if (bad || buf_append(&loc, "", 1) < 0 ||
        http_write_head(&c->out, 301, "text/plain; charset=utf-8", 0, c->keep_alive, buf_ptr(&loc)) < 0) {
        buf_free(&loc);
        conn_close(s, c);
        return -1;
    }
    buf_free(&loc);
    c->status = 301;
    c->state = S_WRITE;
    return 0;
}

static int start_proxy(server *s, conn *c, int upstream_index, const char *body, size_t body_len);

/* body는 이미 다 받은 요청 본문 (프록시가 백엔드로 보낸다. 정적 서버는 쓰지 않는다) */
static int handle_request(server *s, conn *c, const char *body, size_t body_len) {
    const http_request *r = &c->req;
    int head_only = r->method == HTTP_M_HEAD;
    c->keep_alive = r->keep_alive && !s->shutting;

    int ui = router_match(s->cfg, r->path);
    if (ui >= 0) return start_proxy(s, c, ui, body, body_len);

    if (r->method != HTTP_M_GET && !head_only) return respond_405(s, c);

    static_result res;
    static_resolve(s->root, r->path, &res);
    switch (res.status) {
    case 200:
        if (http_write_head(&c->out, 200, mime_type_for_path(res.path), res.size, c->keep_alive, NULL) < 0) {
            close(res.fd);
            conn_close(s, c);
            return -1;
        }
        if (head_only) {
            close(res.fd);
        } else {
            c->file_fd = res.fd;
            c->file_off = 0;
            c->file_end = res.size;
        }
        c->status = 200;
        c->state = S_WRITE;
        return 0;
    case 301: return respond_redirect(s, c);
    default: return respond_error(s, c, res.status, head_only);
    }
}

/* 받아 둔 바이트에서 요청을 하나씩 꺼내 처리한다. 응답이 준비되면(S_WRITE) 멈춘다. */
static void process_input(server *s, conn *c) {
    while (!c->closed) {
        if (c->state == S_READ_HEAD) {
            if (buf_size(&c->in) == 0) return;
            int err = 0;
            http_parse_status st = http_parse_request(buf_ptr(&c->in), buf_size(&c->in), &c->req, &err);
            if (st == HTTP_PARSE_INCOMPLETE) return;
            clock_gettime(CLOCK_MONOTONIC, &c->t0);
            if (st == HTTP_PARSE_ERROR) {
                respond_error(s, c, err, 0);
                return;
            }
            c->have_req = 1;
            c->sent_continue = 0;
            buf_consume(&c->in, c->req.head_len);
            c->state = S_READ_BODY;
        }
        if (c->state == S_READ_BODY) {
            long long need = c->req.content_length > 0 ? c->req.content_length : 0;
            if (need > s->cfg->max_body) {
                respond_error(s, c, 413, 0);
                return;
            }
            if ((long long)buf_size(&c->in) < need) {
                /* 클라이언트가 본문을 보내기 전에 허락을 기다리고 있다 (curl은 큰 본문에서 그렇게 한다) */
                if (!c->sent_continue && http_has_token(http_header_get(&c->req, "Expect"), "100-continue")) {
                    c->sent_continue = 1;
                    if (buf_append_str(&c->out, "HTTP/1.1 100 Continue\r\n\r\n") < 0) conn_close(s, c);
                }
                return;
            }
            handle_request(s, c, buf_ptr(&c->in), (size_t)need);
            if (!c->closed) buf_consume(&c->in, (size_t)need);
            return;
        }
        return;
    }
}

/* out과 파일을 소켓으로 보낸다. 다 보냈으면 1, 소켓이 막혔으면 0, 연결을 닫았으면 -1 */
static int flush_out(server *s, conn *c) {
    for (;;) {
        while (buf_size(&c->out) > 0) {
            ssize_t n = send(c->fd, buf_ptr(&c->out), buf_size(&c->out), MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                conn_close(s, c);
                return -1;
            }
            buf_consume(&c->out, (size_t)n);
            c->bytes_out += n;
            c->last_active = mono();
        }
        if (c->file_fd >= 0 && c->file_off < c->file_end) {
            size_t want = (size_t)(c->file_end - c->file_off);
            if (want > FILE_CHUNK) want = FILE_CHUNK;
            if (buf_reserve(&c->out, want) < 0) {
                conn_close(s, c);
                return -1;
            }
            ssize_t n = pread(c->file_fd, c->out.data + c->out.len, want, c->file_off);
            if (n <= 0) { /* 읽는 도중 파일이 줄었다: Content-Length를 채울 수 없으니 끊는다 */
                conn_close(s, c);
                return -1;
            }
            c->out.len += (size_t)n;
            c->file_off += n;
            continue;
        }
        return 1;
    }
}

/* 응답을 다 보냈다: 기록하고, 연결을 유지할지 닫을지 정한다 */
static void finish_response(server *s, conn *c) {
    log_access(s, c);
    if (c->file_fd >= 0) {
        close(c->file_fd);
        c->file_fd = -1;
    }
    if (c->have_req) {
        http_request_free(&c->req);
        c->have_req = 0;
    }
    if (!c->keep_alive || s->shutting) {
        if (c->linger && !s->shutting) {
            /* 바로 close하면 아직 도착 중인 요청 본문 때문에 RST가 나가 클라이언트가 응답을 못 읽을 수 있다 */
            shutdown(c->fd, SHUT_WR);
            c->state = S_LINGER;
            c->linger_deadline = mono() + LINGER_SECS;
            return;
        }
        conn_close(s, c);
        return;
    }
    c->state = S_READ_HEAD;
    c->status = 0;
    c->bytes_out = 0;
    c->linger = 0;
    c->backend_label = NULL;
    c->resp_started = 0;
    c->last_active = mono();
}

/* 읽은 데이터를 처리하고 응답을 보내는 한 바퀴. 파이프라이닝된 요청은 재귀 없이 이 반복문이 이어서 처리한다. */
static void drive(server *s, conn *c) {
    for (;;) {
        if (c->closed) return;
        if (c->state == S_READ_HEAD || c->state == S_READ_BODY) process_input(s, c);
        if (c->closed) return;
        if (c->state == S_WRITE || buf_size(&c->out) > 0) {
            int r = flush_out(s, c);
            if (r < 0) return;
            if (r == 0) break;
        }
        if (c->state != S_WRITE) break;
        finish_response(s, c);
        if (c->closed) return;
        if (c->state == S_LINGER) break; /* 읽기만 기다린다 (아래에서 관심사를 바꿈) */
    }
    set_interest(s, c);
}

/* ── Reverse Proxy ──
 * 클라이언트 연결(K_CLIENT, S_PROXY)과 백엔드 연결(K_UPSTREAM)이 서로를 peer로 가리킨다.
 * 백엔드 연결: U_CONNECT → U_SEND(요청 전송) → U_HEAD(응답 머리 기다림) → U_RELAY(본문 중계). */

/* 백엔드로 가던 요청이 실패했다. 응답을 아직 쓰기 시작하지 않았으면 오류 응답을, 아니면 연결을 끊는다.
 * 이벤트 핸들러에서 부를 때는 proxy_fail_async를 쓴다 (drive는 호출한 쪽 몫). */
static void proxy_fail(server *s, conn *c, int status) {
    conn *up = c->peer;
    if (up) {
        c->peer = NULL;
        up->peer = NULL;
        conn_close(s, up);
    }
    if (c->resp_started) {
        conn_close(s, c);
        return;
    }
    respond_error(s, c, status, 0);
}

static void proxy_fail_async(server *s, conn *c, int status) {
    proxy_fail(s, c, status);
    if (!c->closed) drive(s, c);
}

/* 백엔드 하나에 non-blocking connect를 건다. 바로 실패하면 다음 백엔드로 넘어가고, 모두 실패하면 502.
 * req(보낼 요청)는 성공하면 백엔드 연결이 넘겨받고, 실패하면 여기서 해제한다. */
static void proxy_connect(server *s, conn *c, buf *req, int bi) {
    upstream *u = &s->cfg->ups[c->up_idx];
    for (;;) {
        backend *b = &u->backends[bi];
        c->attempts++;
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        int started = 0;
        if (fd >= 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            started = connect(fd, (struct sockaddr *)&b->addr, sizeof b->addr) == 0 || errno == EINPROGRESS;
        }
        if (started) {
            conn *up = conn_new(s, fd, NULL);
            if (up) {
                up->peer = c;
                up->up_idx = bi;
                up->out = *req;
                *req = (buf)BUF_INIT;
                c->peer = up;
                c->backend_label = b->label;
                return;
            }
        }
        if (fd >= 0) close(fd);
        router_mark_down(u, bi, mono());
        if (c->attempts >= u->nbackends) {
            buf_free(req);
            proxy_fail(s, c, 502);
            return;
        }
        bi = router_pick(u, mono());
    }
}

static int start_proxy(server *s, conn *c, int upstream_index, const char *body, size_t body_len) {
    upstream *u = &s->cfg->ups[upstream_index];
    int bi = router_pick(u, mono());
    buf req = BUF_INIT;
    if (proxy_build_request(&c->req, c->ip, u->backends[bi].label, &req) < 0 ||
        (body_len > 0 && buf_append(&req, body, body_len) < 0)) {
        buf_free(&req);
        conn_close(s, c);
        return -1;
    }
    c->up_idx = upstream_index;
    c->attempts = 0;
    c->resp_started = 0;
    c->state = S_PROXY;
    c->last_active = mono();
    proxy_connect(s, c, &req, bi);
    return 0;
}

/* 연결을 못 맺었다(거절, 타임아웃). 그 백엔드를 잠시 건너뛰고 다음 백엔드로 같은 요청을 다시 보낸다.
 * 요청을 보내기 전이므로 재시도해도 POST가 두 번 처리될 일이 없다. */
static void connect_failed(server *s, conn *up) {
    conn *c = up->peer;
    upstream *u = &s->cfg->ups[c->up_idx];
    router_mark_down(u, up->up_idx, mono());
    buf req = up->out;
    up->out = (buf)BUF_INIT;
    c->peer = NULL;
    up->peer = NULL;
    conn_close(s, up);
    if (c->attempts >= u->nbackends) {
        buf_free(&req);
        proxy_fail_async(s, c, 502);
        return;
    }
    proxy_connect(s, c, &req, router_pick(u, mono()));
    if (!c->closed) drive(s, c);
}

/* 백엔드 응답을 다 중계했다: 백엔드 연결만 닫고, 클라이언트 쪽은 남은 출력을 보내고 마무리한다 */
static void relay_complete(server *s, conn *up) {
    conn *c = up->peer;
    up->peer = NULL;
    c->peer = NULL;
    conn_close(s, up);
    c->state = S_WRITE;
    drive(s, c);
}

/* 백엔드에서 받은 본문 조각을 클라이언트 출력에 붙인다. 다 중계했으면 1, 아직이면 0, 연결이 닫혔으면 -1 */
static int relay_data(server *s, conn *up, const char *p, size_t n) {
    conn *c = up->peer;
    if (up->relay_remaining >= 0 && (long long)n > up->relay_remaining) n = (size_t)up->relay_remaining;
    if (n > 0 && buf_append(&c->out, p, n) < 0) {
        conn_close(s, c);
        return -1;
    }
    if (up->relay_remaining >= 0) up->relay_remaining -= (long long)n;
    return up->relay_remaining == 0 ? 1 : 0;
}

/* up->in에 응답 머리가 다 왔으면 클라이언트용으로 고쳐 쓰고 본문 중계로 넘어간다 */
static void upstream_got_head(server *s, conn *up) {
    conn *c = up->peer;
    http_response_info info;
    for (;;) {
        http_parse_status st = http_parse_response_head(buf_ptr(&up->in), buf_size(&up->in), &info);
        if (st == HTTP_PARSE_INCOMPLETE) return;
        if (st == HTTP_PARSE_ERROR) {
            proxy_fail_async(s, c, 502);
            return;
        }
        if (info.status >= 100 && info.status < 200 && info.status != 101) { /* 중간 응답(1xx)은 건너뛴다 */
            buf_consume(&up->in, info.head_len);
            continue;
        }
        break;
    }
    int nobody = proxy_response_has_no_body((int)c->req.method, info.status);
    /* 길이를 알 수 없는 응답(chunked, 닫힐 때까지)은 중계한 뒤 클라이언트 연결을 닫는다 */
    int framed = nobody || (!info.chunked && info.content_length >= 0);
    c->keep_alive = c->req.keep_alive && !s->shutting && framed;
    if (proxy_rewrite_response_head(buf_ptr(&up->in), info.head_len, &info, c->keep_alive, &c->out) < 0) {
        conn_close(s, c);
        return;
    }
    c->status = info.status;
    c->resp_started = 1;
    buf_consume(&up->in, info.head_len);
    up->relay_remaining = nobody ? 0 : (info.chunked ? -1 : info.content_length);
    up->ustate = U_RELAY;
    int done = nobody;
    if (!done) {
        int r = relay_data(s, up, buf_ptr(&up->in), buf_size(&up->in));
        if (r < 0) return;
        done = r;
    }
    buf_clear(&up->in);
    if (done) {
        relay_complete(s, up);
        return;
    }
    drive(s, c);
}

static int would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

static void on_upstream_event(server *s, conn *up, unsigned ev) {
    conn *c = up->peer;
    if (!c) {
        conn_close(s, up);
        return;
    }
    if (up->ustate == U_CONNECT) {
        if (!(ev & (EV_WRITE | EV_ERR))) return;
        int err = 0;
        socklen_t l = sizeof err;
        if (getsockopt(up->fd, SOL_SOCKET, SO_ERROR, &err, &l) < 0 || err != 0) {
            connect_failed(s, up);
            return;
        }
        up->ustate = U_SEND;
    }
    if (up->ustate == U_SEND) {
        while (buf_size(&up->out) > 0) {
            ssize_t n = send(up->fd, buf_ptr(&up->out), buf_size(&up->out), MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                proxy_fail_async(s, c, 502);
                return;
            }
            buf_consume(&up->out, (size_t)n);
            up->last_active = mono();
        }
        buf_free(&up->out);
        up->ustate = U_HEAD;
        up->last_active = mono();
        set_interest(s, up);
        return;
    }
    if (!(ev & (EV_READ | EV_ERR))) return;
    if (up->ustate == U_HEAD) {
        if (buf_reserve(&up->in, READ_CHUNK) < 0) {
            proxy_fail_async(s, c, 502);
            return;
        }
        ssize_t n = recv(up->fd, up->in.data + up->in.len, READ_CHUNK, 0);
        if (n > 0) {
            up->in.len += (size_t)n;
            up->last_active = mono();
            upstream_got_head(s, up);
        } else if (n == 0 || !would_block()) {
            proxy_fail_async(s, c, 502); /* 응답 머리 전에 끊김 */
        }
        return;
    }
    /* U_RELAY */
    char tmp[READ_CHUNK];
    ssize_t n = recv(up->fd, tmp, sizeof tmp, 0);
    if (n > 0) {
        up->last_active = mono();
        int r = relay_data(s, up, tmp, (size_t)n);
        if (r < 0) return;
        if (r == 1) relay_complete(s, up);
        else drive(s, c);
    } else if (n == 0 && up->relay_remaining < 0) {
        relay_complete(s, up); /* 닫힐 때까지 이어지는 응답은 닫히면 끝 */
    } else if (n == 0 || !would_block()) {
        conn_close(s, c); /* 약속한 길이를 채우지 못하고 끊김: 클라이언트도 끊어 불완전함을 알린다 */
    }
}

/* 백엔드가 upstream_timeout 동안 진전이 없다 */
static void upstream_timed_out(server *s, conn *up) {
    conn *c = up->peer;
    if (!c) {
        conn_close(s, up);
        return;
    }
    switch (up->ustate) {
    case U_CONNECT: connect_failed(s, up); break;
    case U_SEND:
    case U_HEAD: proxy_fail_async(s, c, 504); break;
    case U_RELAY: conn_close(s, c); break;
    }
}

static void on_readable(server *s, conn *c) {
    if (c->state == S_LINGER) {
        char tmp[4096];
        ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) conn_close(s, c);
        return;
    }
    if (buf_reserve(&c->in, READ_CHUNK) < 0) {
        conn_close(s, c);
        return;
    }
    ssize_t n = recv(c->fd, c->in.data + c->in.len, READ_CHUNK, 0);
    if (n > 0) {
        c->in.len += (size_t)n;
        c->last_active = mono();
        drive(s, c);
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        conn_close(s, c);
    }
}

static void on_event(server *s, conn *c, unsigned ev) {
    if (c->state == S_PROXY && (ev & EV_ERR)) { /* 응답을 기다리던 클라이언트가 끊었다 */
        conn_close(s, c);
        return;
    }
    if (c->state == S_WRITE || (ev & EV_WRITE)) {
        if ((ev & (EV_WRITE | EV_ERR)) == 0) return; /* 쓰기 상태에서 읽기 이벤트는 오지 않지만 방어 */
        drive(s, c);
        return;
    }
    if (ev & (EV_READ | EV_ERR)) on_readable(s, c);
}

/* ── accept와 종료 ── */

static void pause_accept(server *s) {
    if (s->paused) return;
    ev_del(s->ep, s->lfd);
    s->paused = 1;
    s->pause_until = mono() + ACCEPT_PAUSE;
}

static void maybe_resume_accept(server *s) {
    if (!s->paused || s->shutting) return;
    if (mono() < s->pause_until || s->nconns >= s->cfg->max_conns) return;
    if (ev_add(s->ep, s->lfd, EV_READ, &s->lconn) == 0) s->paused = 0;
}

static void accept_loop(server *s) {
    for (;;) {
        if (s->nconns >= s->cfg->max_conns) {
            pause_accept(s);
            return;
        }
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int fd = accept(s->lfd, (struct sockaddr *)&sa, &sl);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                fprintf(stderr, "webserver: accept: %s (pausing accept)\n", strerror(errno));
                pause_accept(s);
            }
            return;
        }
        int one = 1;
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            close(fd);
            continue;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (!conn_new(s, fd, &sa)) close(fd);
    }
}

/* 1초에 한 번: 조용한 연결을 닫는다. 종료 중이면 쉬고 있는 연결도 닫는다. */
static void sweep(server *s) {
    time_t now = mono();
    for (conn *c = s->head, *next; c; c = next) {
        next = c->next;
        if (c->closed) continue; /* 이번 훑기 중 짝 연결이 닫을 때 같이 닫힌 것 */
        if (c->kind == K_UPSTREAM) {
            /* 클라이언트가 느려서 읽기를 멈춰 둔 것은 백엔드 탓이 아니다 */
            int held = c->ustate == U_RELAY && c->peer && buf_size(&c->peer->out) >= PROXY_HIGH;
            if (!held && now - c->last_active >= s->cfg->upstream_timeout) upstream_timed_out(s, c);
        } else if (c->state == S_LINGER) {
            if (now >= c->linger_deadline || s->shutting) conn_close(s, c);
        } else if (c->state == S_PROXY && buf_size(&c->out) == 0) {
            /* 백엔드 응답을 기다리는 중: 백엔드 연결의 타임아웃이 맡는다 */
        } else if (now - c->last_active >= s->cfg->client_timeout) {
            conn_close(s, c);
        } else if (s->shutting && c->state == S_READ_HEAD && buf_size(&c->in) == 0) {
            conn_close(s, c);
        }
    }
}

static int listen_on(server *s) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        fprintf(stderr, "webserver: socket: %s\n", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)s->cfg->port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "webserver: bind to port %d failed: %s\n", s->cfg->port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 511) < 0) {
        fprintf(stderr, "webserver: listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    socklen_t sl = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0)
        fprintf(stderr, "webserver: listening on port %d, root %s\n", ntohs(sa.sin_port), s->root);
    return fd;
}

int server_run(config *cfg, const char *root_real, volatile sig_atomic_t *stop) {
    server s;
    memset(&s, 0, sizeof s);
    s.cfg = cfg;
    s.root = root_real;
    s.lfd = -1;
    s.last_sweep = mono();

    s.ep = ev_create();
    if (s.ep < 0) {
        fprintf(stderr, "webserver: epoll_create1: %s\n", strerror(errno));
        return 1;
    }
    s.lfd = listen_on(&s);
    if (s.lfd < 0) {
        close(s.ep);
        return 1;
    }
    s.lconn.fd = s.lfd;
    s.lconn.kind = K_LISTEN;
    if (ev_add(s.ep, s.lfd, EV_READ, &s.lconn) < 0) {
        fprintf(stderr, "webserver: epoll_ctl: %s\n", strerror(errno));
        close(s.lfd);
        close(s.ep);
        return 1;
    }

    ev_event evs[MAX_EVENTS];
    for (;;) {
        if (*stop && !s.shutting) {
            s.shutting = 1;
            s.deadline = mono() + SHUTDOWN_GRACE;
            ev_del(s.ep, s.lfd);
            close(s.lfd);
            s.lfd = -1;
            sweep(&s); /* 쉬고 있는 연결부터 닫는다 */
            free_dead(&s);
        }
        if (s.shutting && (s.nconns == 0 || mono() >= s.deadline)) break;
        maybe_resume_accept(&s);

        int n = ev_wait(s.ep, evs, MAX_EVENTS, 1000);
        if (n < 0) {
            fprintf(stderr, "webserver: epoll_wait: %s\n", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            conn *c = evs[i].ptr;
            if (c->closed) continue;
            if (c->kind == K_LISTEN) accept_loop(&s);
            else if (c->kind == K_UPSTREAM) on_upstream_event(&s, c, evs[i].events);
            else on_event(&s, c, evs[i].events);
        }
        free_dead(&s);

        if (mono() != s.last_sweep) {
            s.last_sweep = mono();
            sweep(&s);
            free_dead(&s);
        }
    }

    while (s.head) conn_close(&s, s.head);
    free_dead(&s);
    if (s.lfd >= 0) close(s.lfd);
    close(s.ep);
    fflush(stdout);
    return 0;
}
