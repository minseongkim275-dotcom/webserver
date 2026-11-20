/* loadgen: 의존성 없는 부하 생성기 (ab/wrk/hey가 없는 환경용).
 * 연결 c개를 스레드 하나씩 맡아 keep-alive로 GET을 반복하고, 처리량과 지연 분포를 출력한다.
 *   loadgen -u http://127.0.0.1:8080/index.html -c 100 -d 5
 * 응답은 Content-Length가 있는 것만 정확히 센다. 그 밖(chunked, 닫힘)은 닫힐 때까지 읽고 다시 연결한다. */
#define _GNU_SOURCE /* strcasestr */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static char g_host[96] = "127.0.0.1";
static char g_path[512] = "/";
static int g_port = 80;
static int g_conns = 10;
static int g_seconds = 5;
static long long g_deadline_us;

typedef struct {
    uint32_t *lat_us; /* 요청마다 지연(마이크로초) */
    size_t n, cap;
    long errors; /* 연결 실패, 중간에 끊김, 형식 오류 */
    long non2xx;
} stats;

static long long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int connect_to(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)g_port);
    inet_pton(AF_INET, g_host, &a.sin_addr);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 응답 하나를 끝까지 읽는다. 성공하면 상태 코드, 연결을 계속 쓸 수 없으면 *reuse=0. 실패하면 -1.
 * 머리는 buf에 모아 해석하고, 본문은 버리면서 길이만 센다. */
static int read_response(int fd, char *buf, size_t cap, int *reuse) {
    size_t len = 0;
    long long clen = -1, body = 0;
    int status = 0, have_head = 0;
    *reuse = 1;
    for (;;) {
        ssize_t n = have_head ? recv(fd, buf, cap, 0) : recv(fd, buf + len, cap - 1 - len, 0);
        if (n <= 0) return have_head && clen < 0 ? status : -1; /* 닫힐 때까지 이어지는 응답은 닫히면 끝 */
        if (!have_head) {
            len += (size_t)n;
            buf[len] = 0;
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) {
                if (len + 1 >= cap) return -1; /* 머리가 너무 길다 */
                continue;
            }
            have_head = 1;
            status = atoi(buf + 9);
            for (char *p = buf; p < end; p++)
                if (strncasecmp(p, "\r\nContent-Length:", 17) == 0) clen = atoll(p + 17);
            if (clen < 0 || strcasestr(buf, "\r\nConnection: close")) *reuse = 0;
            body = (long long)(len - (size_t)(end + 4 - buf));
        } else {
            body += n;
        }
        if (clen >= 0 && body >= clen) return status;
    }
}

static void *worker(void *arg) {
    stats *st = arg;
    char req[1024];
    int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\n\r\n", g_path, g_host, g_port);
    char *buf = malloc(65536);
    int fd = -1;
    while (now_us() < g_deadline_us) {
        if (fd < 0 && (fd = connect_to()) < 0) {
            st->errors++;
            usleep(1000);
            continue;
        }
        long long t0 = now_us();
        int reuse = 1, status = -1;
        if (send(fd, req, (size_t)rl, MSG_NOSIGNAL) == rl) status = read_response(fd, buf, 65536, &reuse);
        if (status < 0) {
            st->errors++;
            close(fd);
            fd = -1;
            continue;
        }
        if (status < 200 || status >= 300) st->non2xx++;
        if (st->n == st->cap) {
            st->cap = st->cap ? st->cap * 2 : 4096;
            st->lat_us = realloc(st->lat_us, st->cap * sizeof *st->lat_us);
        }
        st->lat_us[st->n++] = (uint32_t)(now_us() - t0);
        if (!reuse) {
            close(fd);
            fd = -1;
        }
    }
    if (fd >= 0) close(fd);
    free(buf);
    return NULL;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int usage(void) {
    fprintf(stderr, "usage: loadgen -u http://127.0.0.1:8080/path [-c conns] [-d seconds]\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) url = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) g_conns = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) g_seconds = atoi(argv[++i]);
        else return usage();
    }
    if (!url || strncmp(url, "http://", 7) != 0 || g_conns < 1 || g_seconds < 1) return usage();
    const char *hp = url + 7, *slash = strchr(hp, '/');
    size_t hl = slash ? (size_t)(slash - hp) : strlen(hp);
    char hostport[96];
    snprintf(hostport, sizeof hostport, "%.*s", (int)hl, hp);
    snprintf(g_path, sizeof g_path, "%s", slash ? slash : "/");
    char *colon = strchr(hostport, ':');
    if (colon) {
        *colon = '\0';
        g_port = atoi(colon + 1);
    }
    snprintf(g_host, sizeof g_host, "%s", hostport);

    long long t_start = now_us();
    g_deadline_us = t_start + (long long)g_seconds * 1000000;

    pthread_t *th = calloc((size_t)g_conns, sizeof *th);
    stats *st = calloc((size_t)g_conns, sizeof *st);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    int started = 0;
    for (int i = 0; i < g_conns; i++) {
        if (pthread_create(&th[i], &attr, worker, &st[i]) != 0) break;
        started++;
    }
    if (started < g_conns) fprintf(stderr, "loadgen: only %d of %d threads started\n", started, g_conns);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    double secs = (double)(now_us() - t_start) / 1e6;

    size_t total = 0;
    long errors = 0, non2xx = 0;
    for (int i = 0; i < g_conns; i++) {
        total += st[i].n;
        errors += st[i].errors;
        non2xx += st[i].non2xx;
    }
    uint32_t *all = malloc((total ? total : 1) * sizeof *all), *w = all;
    for (int i = 0; i < g_conns; i++) {
        if (st[i].n) {
            memcpy(w, st[i].lat_us, st[i].n * sizeof *w);
            w += st[i].n;
        }
        free(st[i].lat_us);
    }
    qsort(all, total, sizeof *all, cmp_u32);
#define PCT(p) (total ? all[(size_t)((double)(total - 1) * (p))] / 1000.0 : 0.0)
    printf("c=%-5d requests=%-8zu rps=%-9.0f p50=%.2fms p90=%.2fms p99=%.2fms max=%.2fms errors=%ld non2xx=%ld\n", g_conns,
           total, (double)total / secs, PCT(0.50), PCT(0.90), PCT(0.99), PCT(1.0), errors, non2xx);
    free(all);
    free(st);
    free(th);
    return errors || non2xx ? 1 : 0;
}
