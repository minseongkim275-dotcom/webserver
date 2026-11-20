/* 데모 백엔드: Reverse Proxy 뒤에 두는 작은 HTTP 서버 (동적 요청 처리 예시이자 테스트 상대역).
 * 연결마다 스레드를 하나 만드는 단순한 구조다. 웹 서버 본체가 아니다.
 *
 *   GET  /api/hello            {"backend":"이름","count":N}
 *   POST /api/echo             받은 본문을 그대로 돌려준다
 *   GET  /api/sum?a=1&b=2      {"sum":3}
 *   GET  /api/slow?ms=N        N밀리초 뒤에 응답 (타임아웃 테스트)
 *   GET  /api/chunked          Transfer-Encoding: chunked 응답
 *   GET  /api/big?kb=N         N KB짜리 본문 (느린 클라이언트·역압 테스트)
 *   GET  /api/headers          받은 요청 헤더를 한 줄씩 돌려준다 (프록시가 붙인 헤더 확인용)
 *   GET  /api/drop             응답 없이 연결을 닫음
 *   그 밖                       404
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "buf.h"
#include "http.h"

#define MAX_BODY (1024 * 1024)

static const char *g_name = "backend";
static atomic_ulong g_count;
static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static int send_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void reply(int fd, int status, const char *type, const char *body, size_t len) {
    buf out = BUF_INIT;
    if (http_write_head(&out, status, type, (long long)len, 0, NULL) == 0 && buf_append(&out, body, len) == 0)
        send_all(fd, buf_ptr(&out), buf_size(&out));
    buf_free(&out);
}

/* 쿼리에서 name=값의 숫자를 읽는다. 없거나 형식이 틀리면 def */
static long query_num(const char *query, const char *name, long def) {
    if (!query) return def;
    size_t nl = strlen(name);
    for (const char *p = query; *p;) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            char *end;
            long v = strtol(p + nl + 1, &end, 10);
            return end == p + nl + 1 ? def : v;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    return def;
}

static void handle(int fd, const http_request *req, const char *body, size_t body_len) {
    char tmp[256];
    const char *path = req->path;

    if (strcmp(path, "/api/hello") == 0) {
        unsigned long n = atomic_fetch_add(&g_count, 1) + 1;
        int len = snprintf(tmp, sizeof tmp, "{\"backend\":\"%s\",\"count\":%lu}", g_name, n);
        reply(fd, 200, "application/json", tmp, (size_t)len);
    } else if (strcmp(path, "/api/echo") == 0) {
        reply(fd, 200, "application/octet-stream", body, body_len);
    } else if (strcmp(path, "/api/sum") == 0) {
        int len = snprintf(tmp, sizeof tmp, "{\"sum\":%ld}", query_num(req->query, "a", 0) + query_num(req->query, "b", 0));
        reply(fd, 200, "application/json", tmp, (size_t)len);
    } else if (strcmp(path, "/api/slow") == 0) {
        usleep((useconds_t)query_num(req->query, "ms", 1000) * 1000);
        reply(fd, 200, "text/plain", "slow\n", 5);
    } else if (strcmp(path, "/api/chunked") == 0) {
        static const char resp[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n"
                                   "Connection: close\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        send_all(fd, resp, sizeof resp - 1);
    } else if (strcmp(path, "/api/big") == 0) {
        long kb = query_num(req->query, "kb", 1);
        if (kb < 0 || kb > 200000) kb = 1;
        buf head = BUF_INIT;
        if (http_write_head(&head, 200, "application/octet-stream", kb * 1024, 0, NULL) == 0 &&
            send_all(fd, buf_ptr(&head), buf_size(&head)) == 0) {
            char chunk[1024];
            memset(chunk, 'x', sizeof chunk);
            for (long i = 0; i < kb; i++)
                if (send_all(fd, chunk, sizeof chunk) < 0) break;
        }
        buf_free(&head);
    } else if (strcmp(path, "/api/headers") == 0) {
        buf b = BUF_INIT;
        for (size_t i = 0; i < req->nheaders; i++) buf_printf(&b, "%s: %s\n", req->headers[i].name, req->headers[i].value);
        reply(fd, 200, "text/plain", buf_ptr(&b) ? buf_ptr(&b) : "", buf_size(&b));
        buf_free(&b);
    } else if (strcmp(path, "/api/drop") == 0) {
        /* 아무것도 보내지 않고 닫는다 */
    } else {
        reply(fd, 404, "application/json", "{\"error\":\"not found\"}", 21);
    }
}

static void *serve(void *arg) {
    int fd = (int)(intptr_t)arg;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    buf in = BUF_INIT;
    http_request req;
    int err = 0;
    for (;;) {
        http_parse_status st = http_parse_request(buf_ptr(&in), buf_size(&in), &req, &err);
        if (st == HTTP_PARSE_OK) break;
        if (st == HTTP_PARSE_ERROR) {
            buf out = BUF_INIT;
            if (http_write_error(&out, err, 0, 0) == 0) send_all(fd, buf_ptr(&out), buf_size(&out));
            buf_free(&out);
            goto done;
        }
        if (buf_reserve(&in, 4096) < 0) goto done;
        ssize_t n = recv(fd, in.data + in.len, 4096, 0);
        if (n <= 0) goto done;
        in.len += (size_t)n;
    }
    long long need = req.content_length > 0 ? req.content_length : 0;
    if (need > MAX_BODY) {
        buf out = BUF_INIT;
        if (http_write_error(&out, 413, 0, 0) == 0) send_all(fd, buf_ptr(&out), buf_size(&out));
        buf_free(&out);
    } else {
        buf_consume(&in, req.head_len);
        while ((long long)buf_size(&in) < need) {
            if (buf_reserve(&in, 4096) < 0) goto free_req;
            ssize_t n = recv(fd, in.data + in.len, 4096, 0);
            if (n <= 0) goto free_req;
            in.len += (size_t)n;
        }
        handle(fd, &req, buf_ptr(&in), (size_t)need);
    }
free_req:
    http_request_free(&req);
done:
    buf_free(&in);
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int port = 9001;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) g_name = argv[++i];
        else {
            fprintf(stderr, "usage: backend [-p port] [-n name]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 128) < 0) {
        fprintf(stderr, "backend %s: cannot listen on %d: %s\n", g_name, port, strerror(errno));
        return 1;
    }
    fprintf(stderr, "backend %s: listening on 127.0.0.1:%d\n", g_name, port);

    while (!g_stop) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) continue; /* 시그널로 깨어나면 g_stop을 본다 */
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, serve, (void *)(intptr_t)fd) != 0) close(fd);
        pthread_attr_destroy(&attr);
    }
    close(lfd);
    return 0;
}
