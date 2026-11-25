/* config: 설정 파일과 CLI 인자를 읽는다. 소켓은 만지지 않는다.
 * 백엔드 주소는 IP:포트만 받는다 (이름 풀이는 블로킹이라 이벤트 루프 안에서 하지 않는다). */
#ifndef WS_CONFIG_H
#define WS_CONFIG_H

#include <limits.h>
#include <netinet/in.h>
#include <time.h>

#define CFG_MAX_UPSTREAMS 16
#define CFG_MAX_BACKENDS 16
#define CFG_MAX_ROUTES 32
#define CFG_MAX_WORKERS 256
#define CFG_NAME_LEN 32
#define CFG_PREFIX_LEN 256

typedef struct {
    struct sockaddr_in addr;
    char label[32];  /* "127.0.0.1:9001" (로그와 Host 대체값) */
    time_t down_until; /* 연결 실패 뒤 이 시각까지는 고르지 않는다 */
} backend;

typedef struct {
    char name[CFG_NAME_LEN];
    backend backends[CFG_MAX_BACKENDS];
    int nbackends;
    unsigned next; /* Round-Robin 커서 */
} upstream;

typedef struct {
    char prefix[CFG_PREFIX_LEN];
    char target[CFG_NAME_LEN]; /* upstream 이름 또는 "static" */
    int upstream;              /* config_finish가 채움. -1이면 정적 파일 */
} route;

typedef struct {
    int port;
    char root[PATH_MAX];
    long long max_body;
    int client_timeout;   /* 초 */
    int upstream_timeout; /* 초 */
    int max_conns;        /* 워커 모드에서는 워커 하나당 한도 */
    int workers;          /* 1이면 master 없이 프로세스 하나. 2 이상이면 master + 워커 N개 */
    int quiet;            /* 접근 로그 끄기 */
    upstream ups[CFG_MAX_UPSTREAMS];
    int nups;
    route routes[CFG_MAX_ROUTES];
    int nroutes;
} config;

void config_defaults(config *c);
/* 한 줄을 적용한다. 주석(#)과 빈 줄은 무시. 실패하면 -1과 err 메시지 */
int config_apply_line(config *c, const char *line, char *err, size_t errlen);
int config_load_file(config *c, const char *path, char *err, size_t errlen);
/* -c 파일을 먼저 읽고, 나머지 인자(-p, -r, -q)로 덮어쓴 뒤 config_finish까지 한다.
 * -h면 1을 돌려준다 (사용법 출력은 호출한 쪽이). 오류면 -1 */
int config_from_args(config *c, int argc, char **argv, char *err, size_t errlen);
/* route가 가리키는 upstream 이름을 번호로 바꾸고 값의 범위를 검사한다 */
int config_finish(config *c, char *err, size_t errlen);
/* "1.2.3.4:80" → backend. 실패하면 -1 */
int config_parse_backend(const char *s, backend *b);

#endif
