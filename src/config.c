#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(char *err, size_t errlen, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return -1;
}

void config_defaults(config *c) {
    memset(c, 0, sizeof *c);
    c->port = 8080;
    snprintf(c->root, sizeof c->root, "www");
    c->max_body = 1024 * 1024;
    c->client_timeout = 15;
    c->upstream_timeout = 5;
    c->max_conns = 10000;
    c->workers = 1;
}

/* 10진 정수만, 범위 [lo, hi] */
static int parse_num(const char *s, long long lo, long long hi, long long *out) {
    if (!*s) return -1;
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || *end || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

int config_parse_backend(const char *s, backend *b) {
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s || (size_t)(colon - s) >= 16) return -1;
    char ip[16];
    memcpy(ip, s, (size_t)(colon - s));
    ip[colon - s] = '\0';
    long long port;
    if (parse_num(colon + 1, 1, 65535, &port) < 0) return -1;
    memset(b, 0, sizeof *b);
    b->addr.sin_family = AF_INET;
    b->addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &b->addr.sin_addr) != 1) return -1;
    snprintf(b->label, sizeof b->label, "%s:%lld", ip, port);
    return 0;
}

#define MAX_WORDS 2 + CFG_MAX_BACKENDS

int config_apply_line(config *c, const char *line, char *err, size_t errlen) {
    char tmp[1024];
    if (strlen(line) >= sizeof tmp) return fail(err, errlen, "line too long");
    snprintf(tmp, sizeof tmp, "%s", line);
    char *hash = strchr(tmp, '#');
    if (hash) *hash = '\0';

    char *w[MAX_WORDS + 1];
    int n = 0;
    for (char *save = NULL, *t = strtok_r(tmp, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
        if (n == MAX_WORDS + 1) return fail(err, errlen, "too many words");
        w[n++] = t;
    }
    if (n == 0) return 0;

    long long v;
    const char *k = w[0];
    if (strcmp(k, "listen") == 0 || strcmp(k, "max_body") == 0 || strcmp(k, "client_timeout") == 0 ||
        strcmp(k, "upstream_timeout") == 0 || strcmp(k, "max_conns") == 0) {
        if (n != 2) return fail(err, errlen, "%s: expects one value", k);
        long long hi = strcmp(k, "listen") == 0 ? 65535 : strcmp(k, "max_body") == 0 ? (1LL << 40) : 1000000;
        long long lo = strcmp(k, "listen") == 0 ? 0 : 1;
        if (parse_num(w[1], lo, hi, &v) < 0) return fail(err, errlen, "%s: bad number '%s'", k, w[1]);
        if (strcmp(k, "listen") == 0) c->port = (int)v;
        else if (strcmp(k, "max_body") == 0) c->max_body = v;
        else if (strcmp(k, "client_timeout") == 0) c->client_timeout = (int)v;
        else if (strcmp(k, "upstream_timeout") == 0) c->upstream_timeout = (int)v;
        else c->max_conns = (int)v;
        return 0;
    }
    if (strcmp(k, "workers") == 0) {
        if (n != 2) return fail(err, errlen, "workers: expects a number or 'auto'");
        if (strcmp(w[1], "auto") == 0) {
            long cores = sysconf(_SC_NPROCESSORS_ONLN);
            v = cores < 1 ? 1 : cores > CFG_MAX_WORKERS ? CFG_MAX_WORKERS : cores;
        } else if (parse_num(w[1], 1, CFG_MAX_WORKERS, &v) < 0) {
            return fail(err, errlen, "workers: bad number '%s' (1..%d or auto)", w[1], CFG_MAX_WORKERS);
        }
        c->workers = (int)v;
        return 0;
    }
    if (strcmp(k, "root") == 0) {
        if (n != 2) return fail(err, errlen, "root: expects one path");
        if (strlen(w[1]) >= sizeof c->root) return fail(err, errlen, "root: path too long");
        snprintf(c->root, sizeof c->root, "%s", w[1]);
        return 0;
    }
    if (strcmp(k, "upstream") == 0) {
        if (n < 3) return fail(err, errlen, "upstream: expects a name and at least one ip:port");
        if (n - 2 > CFG_MAX_BACKENDS) return fail(err, errlen, "upstream: too many backends");
        if (strlen(w[1]) >= CFG_NAME_LEN || strcmp(w[1], "static") == 0)
            return fail(err, errlen, "upstream: bad name '%s'", w[1]);
        for (int i = 0; i < c->nups; i++)
            if (strcmp(c->ups[i].name, w[1]) == 0) return fail(err, errlen, "upstream: duplicate name '%s'", w[1]);
        if (c->nups == CFG_MAX_UPSTREAMS) return fail(err, errlen, "upstream: too many upstreams");
        upstream *u = &c->ups[c->nups];
        memset(u, 0, sizeof *u);
        snprintf(u->name, sizeof u->name, "%s", w[1]);
        for (int i = 2; i < n; i++)
            if (config_parse_backend(w[i], &u->backends[u->nbackends++]) < 0)
                return fail(err, errlen, "upstream %s: bad address '%s' (want ip:port)", w[1], w[i]);
        c->nups++;
        return 0;
    }
    if (strcmp(k, "route") == 0) {
        if (n != 3) return fail(err, errlen, "route: expects a path prefix and a target");
        if (w[1][0] != '/' || strlen(w[1]) >= CFG_PREFIX_LEN) return fail(err, errlen, "route: bad prefix '%s'", w[1]);
        if (strlen(w[2]) >= CFG_NAME_LEN) return fail(err, errlen, "route: bad target '%s'", w[2]);
        if (c->nroutes == CFG_MAX_ROUTES) return fail(err, errlen, "route: too many routes");
        route *r = &c->routes[c->nroutes++];
        snprintf(r->prefix, sizeof r->prefix, "%s", w[1]);
        snprintf(r->target, sizeof r->target, "%s", w[2]);
        r->upstream = -1;
        return 0;
    }
    return fail(err, errlen, "unknown directive '%s'", k);
}

int config_load_file(config *c, const char *path, char *err, size_t errlen) {
    FILE *f = fopen(path, "r");
    if (!f) return fail(err, errlen, "%s: %s", path, strerror(errno));
    char line[1024], msg[256];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            rc = fail(err, errlen, "%s:%d: line too long", path, lineno);
            break;
        }
        if (config_apply_line(c, line, msg, sizeof msg) < 0) {
            rc = fail(err, errlen, "%s:%d: %s", path, lineno, msg);
            break;
        }
    }
    fclose(f);
    return rc;
}

int config_finish(config *c, char *err, size_t errlen) {
    for (int i = 0; i < c->nroutes; i++) {
        route *r = &c->routes[i];
        r->upstream = -1;
        if (strcmp(r->target, "static") == 0) continue;
        for (int j = 0; j < c->nups; j++)
            if (strcmp(c->ups[j].name, r->target) == 0) r->upstream = j;
        if (r->upstream < 0) return fail(err, errlen, "route %s: unknown upstream '%s'", r->prefix, r->target);
    }
    if (c->root[0] == '\0') return fail(err, errlen, "root is empty");
    return 0;
}

int config_from_args(config *c, int argc, char **argv, char *err, size_t errlen) {
    /* 설정 파일을 먼저 읽어야 CLI 값이 그 위를 덮어쓸 수 있으므로 -c부터 찾는다 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) return fail(err, errlen, "-c needs a file");
            if (config_load_file(c, argv[i + 1], err, errlen) < 0) return -1;
        }
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) return 1;
        if (strcmp(a, "-q") == 0) {
            c->quiet = 1;
            continue;
        }
        if (strcmp(a, "-c") != 0 && strcmp(a, "-p") != 0 && strcmp(a, "-r") != 0)
            return fail(err, errlen, "unknown option '%s'", a);
        if (i + 1 >= argc) return fail(err, errlen, "%s needs a value", a);
        const char *v = argv[++i];
        if (strcmp(a, "-p") == 0) {
            long long port;
            if (parse_num(v, 0, 65535, &port) < 0) return fail(err, errlen, "-p: bad port '%s'", v);
            c->port = (int)port;
        } else if (strcmp(a, "-r") == 0) {
            if (strlen(v) >= sizeof c->root) return fail(err, errlen, "-r: path too long");
            snprintf(c->root, sizeof c->root, "%s", v);
        }
    }
    return config_finish(c, err, errlen);
}
