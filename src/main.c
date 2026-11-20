#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "server.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1; /* 핸들러에서는 플래그만 세운다. 정리는 이벤트 루프가 한다 */
}

static void usage(FILE *out) {
    fprintf(out,
            "usage: webserver [-c conf] [-p port] [-r root] [-q]\n"
            "  -c conf   설정 파일 (conf/webserver.conf 참고)\n"
            "  -p port   포트 (기본 8080, 설정 파일보다 우선)\n"
            "  -r root   정적 파일 폴더 (기본 www, 설정 파일보다 우선)\n"
            "  -q        접근 로그를 출력하지 않음\n"
            "  -h        이 도움말\n");
}

int main(int argc, char **argv) {
    config cfg;
    char err[256];
    config_defaults(&cfg);
    int r = config_from_args(&cfg, argc, argv, err, sizeof err);
    if (r == 1) {
        usage(stdout);
        return 0;
    }
    if (r < 0) {
        fprintf(stderr, "webserver: %s\n", err);
        usage(stderr);
        return 2;
    }

    char root[PATH_MAX];
    struct stat st;
    if (!realpath(cfg.root, root) || stat(root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "webserver: root '%s' is not a directory\n", cfg.root);
        return 1;
    }

    /* 접근 로그는 stdout으로 나간다. 파일이나 파이프로 보내도 서버가 도는 중에 한 줄씩 보이도록 줄 단위 버퍼링 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 쓰는 도중 클라이언트가 끊어도 프로세스가 죽지 않게 한다. (send에도 MSG_NOSIGNAL을 준다) */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* SA_RESTART를 주지 않는다: epoll_wait가 EINTR로 깨어나 바로 플래그를 본다 */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    return server_run(&cfg, root, &g_stop);
}
