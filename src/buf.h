/* buf: 필요한 만큼 늘어나는 바이트 버퍼.
 * 앞쪽에서 소비한 만큼은 off로 건너뛰고, 공간이 모자랄 때만 앞으로 당긴다(memmove).
 * 소켓으로 조금씩 보내고 남은 데이터를 매번 당기지 않기 위해서다. */
#ifndef WS_BUF_H
#define WS_BUF_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t off; /* 이미 소비한 앞부분 길이 */
    size_t len; /* data 안에서 유효한 끝 위치 (off <= len <= cap) */
    size_t cap;
} buf;

#define BUF_INIT {NULL, 0, 0, 0}

/* 남은 데이터의 시작과 길이 */
static inline char *buf_ptr(const buf *b) { return b->data + b->off; }
static inline size_t buf_size(const buf *b) { return b->len - b->off; }

/* 끝에 extra 바이트를 더 쓸 수 있게 공간을 확보한다. 실패하면 -1 (버퍼는 그대로). */
int buf_reserve(buf *b, size_t extra);
int buf_append(buf *b, const void *p, size_t n);
int buf_append_str(buf *b, const char *s);
int buf_printf(buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* 앞에서 n 바이트를 소비한다. 다 소비하면 위치를 처음으로 되돌린다. */
void buf_consume(buf *b, size_t n);
void buf_clear(buf *b);
void buf_free(buf *b);

#endif
