#include "buf.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int buf_reserve(buf *b, size_t extra) {
    if (b->cap - b->len >= extra) return 0;

    /* 앞에 소비한 공간을 당기면 충분한 경우: 새로 할당하지 않는다. */
    size_t used = b->len - b->off;
    if (b->off > 0 && b->cap - used >= extra) {
        memmove(b->data, b->data + b->off, used);
        b->off = 0;
        b->len = used;
        return 0;
    }

    if (extra > SIZE_MAX - used) return -1;
    size_t need = used + extra;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    char *p = malloc(cap);
    if (!p) return -1;
    if (used) memcpy(p, b->data + b->off, used);
    free(b->data);
    b->data = p;
    b->off = 0;
    b->len = used;
    b->cap = cap;
    return 0;
}

int buf_append(buf *b, const void *p, size_t n) {
    if (n == 0) return 0;
    if (buf_reserve(b, n) < 0) return -1;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

int buf_append_str(buf *b, const char *s) { return buf_append(b, s, strlen(s)); }

int buf_printf(buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    /* vsnprintf는 끝에 NUL을 쓰므로 한 바이트 더 확보한다. len에는 넣지 않는다. */
    if (buf_reserve(b, (size_t)n + 1) < 0) return -1;
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

void buf_consume(buf *b, size_t n) {
    size_t have = buf_size(b);
    if (n >= have) {
        b->off = b->len = 0;
        return;
    }
    b->off += n;
}

void buf_clear(buf *b) { b->off = b->len = 0; }

void buf_free(buf *b) {
    free(b->data);
    b->data = NULL;
    b->off = b->len = b->cap = 0;
}
