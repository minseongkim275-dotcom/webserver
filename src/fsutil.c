#include "fsutil.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_decode_path(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        char c = in[i];
        if (c == '%') {
            int hi = hexval(in[i + 1]);
            int lo = hi < 0 ? -1 : hexval(in[i + 2]);
            if (hi < 0 || lo < 0) return -1;
            c = (char)(hi * 16 + lo);
            if (c == '\0') return -1;
            i += 2;
        }
        if (o + 1 >= outsz) return -1;
        out[o++] = c;
    }
    out[o] = '\0';
    return 0;
}

/* 경로 조각 중 '.'으로 시작하는 것이 있는가 (".", "..", ".git", ".env" 모두) */
static int has_dot_segment(const char *p) {
    for (const char *s = p; *s; s++)
        if (s[0] == '/' && s[1] == '.') return 1;
    return 0;
}

static int status_from_errno(int e) {
    switch (e) {
    case ENOENT:
    case ENOTDIR: return 404;
    case EACCES:
    case EPERM:
    case ELOOP: return 403;
    case ENAMETOOLONG: return 414;
    default: return 500;
    }
}

void static_resolve(const char *root_real, const char *url_path, static_result *res) {
    char dec[PATH_MAX], full[PATH_MAX], real[PATH_MAX];
    res->fd = -1;
    res->size = 0;
    res->path[0] = '\0';

    if (url_path[0] != '/') {
        res->status = 400;
        return;
    }
    if (url_decode_path(url_path, dec, sizeof dec) < 0) {
        res->status = strlen(url_path) >= sizeof dec ? 414 : 400;
        return;
    }
    if (has_dot_segment(dec)) {
        res->status = 403;
        return;
    }
    size_t dl = strlen(dec);
    int wants_dir = dec[dl - 1] == '/';
    int n = snprintf(full, sizeof full, "%s%s%s", root_real, dec, wants_dir ? "index.html" : "");
    if (n < 0 || (size_t)n >= sizeof full) {
        res->status = 414;
        return;
    }

    /* 심볼릭 링크를 모두 따라간 실제 경로가 root 안인지 본다 */
    if (!realpath(full, real)) {
        res->status = status_from_errno(errno);
        return;
    }
    size_t rl = strlen(root_real);
    if (strncmp(real, root_real, rl) != 0 || (real[rl] != '/' && real[rl] != '\0')) {
        res->status = 403;
        return;
    }

    /* O_NONBLOCK: FIFO 같은 특수 파일을 열 때 멈추지 않게 */
    int fd = open(real, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        res->status = status_from_errno(errno);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        res->status = 500;
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        res->status = wants_dir ? 403 : 301;
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        res->status = 403;
        return;
    }
    /* 일반 파일은 다시 blocking으로 (pread는 어차피 바로 끝나지만 의도를 분명히) */
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

    res->status = 200;
    res->fd = fd;
    res->size = st.st_size;
    snprintf(res->path, sizeof res->path, "%s", real);
}
