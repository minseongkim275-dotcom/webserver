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

#define READ_CHUNK 16384
#define FILE_CHUNK 65536
#define MAX_EVENTS 256
#define LINGER_SECS 2    /* 오류 응답 뒤 클라이언트가 보내는 나머지를 버리며 기다리는 시간 */
#define SHUTDOWN_GRACE 5 /* 종료 신호 뒤 진행 중인 응답을 기다리는 최대 시간(초) */
#define ACCEPT_PAUSE 1   /* fd가 모자라거나 연결이 한도에 닿았을 때 accept를 쉬는 시간(초) */

typedef enum { K_LISTEN, K_CLIENT } kind_t;
typedef enum { S_READ_HEAD, S_READ_BODY, S_WRITE, S_LINGER } state_t;

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
    struct conn *prev, *next; /* 살아 있는 연결 목록 */
    struct conn *dead_next;   /* 지연 해제 목록 */
    int closed;
} conn;

typedef struct {
    const config *cfg;
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
    if (c->state == S_WRITE || buf_size(&c->out) > 0) want |= EV_WRITE;
    if (c->state != S_WRITE) want |= EV_READ;
    if (want == c->interest) return;
    c->interest = want;
    if (ev_mod(s->ep, c->fd, want, c) < 0) c->interest = (unsigned)-1; /* 다음에 다시 시도 */
}

/* 닫는다. 구조체는 이번 이벤트 묶음이 끝난 뒤에 해제한다.
 * (같은 묶음 안에 이 연결의 이벤트가 더 남아 있어도 해제된 메모리를 읽지 않게 하려는 것이다.) */
static void conn_close(server *s, conn *c) {
    if (c->closed) return;
    c->closed = 1;
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

static conn *conn_new(server *s, int fd, const struct sockaddr_in *peer) {
    conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = fd;
    c->kind = K_CLIENT;
    c->state = S_READ_HEAD;
    c->file_fd = -1;
    c->last_active = mono();
    inet_ntop(AF_INET, &peer->sin_addr, c->ip, sizeof c->ip);
    c->interest = EV_READ;
    if (ev_add(s->ep, fd, EV_READ, c) < 0) {
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
        printf("%s \"%s %s HTTP/%d.%d\" %d %lld %ldms\n", c->ip, c->req.method_str, c->req.target,
               c->req.version / 10, c->req.version % 10, c->status, c->bytes_out, ms_since(&c->t0));
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

static int handle_request(server *s, conn *c) {
    const http_request *r = &c->req;
    int head_only = r->method == HTTP_M_HEAD;
    c->keep_alive = r->keep_alive && !s->shutting;

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
            buf_consume(&c->in, (size_t)need); /* 정적 서버는 본문을 쓰지 않는다 */
            handle_request(s, c);
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
        if (c->state == S_LINGER) {
            if (now >= c->linger_deadline || s->shutting) conn_close(s, c);
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

int server_run(const config *cfg, const char *root_real, volatile sig_atomic_t *stop) {
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
