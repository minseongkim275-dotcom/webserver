#include "router.h"

#include <string.h>

int router_match(const config *c, const char *path) {
    size_t plen = strlen(path);
    int best = -2; /* -2: 맞는 route 없음 */
    size_t best_len = 0;
    for (int i = 0; i < c->nroutes; i++) {
        const route *r = &c->routes[i];
        size_t n = strlen(r->prefix);
        if (n > plen || strncmp(path, r->prefix, n) != 0) continue;
        int boundary = r->prefix[n - 1] == '/' || path[n] == '\0' || path[n] == '/';
        if (!boundary) continue;
        if (best == -2 || n > best_len) {
            best = r->upstream;
            best_len = n;
        }
    }
    return best == -2 ? -1 : best;
}

int router_pick(upstream *u, time_t now) {
    int n = u->nbackends;
    for (int k = 0; k < n; k++) {
        int idx = (int)((u->next + (unsigned)k) % (unsigned)n);
        if (u->backends[idx].down_until <= now) {
            u->next = (unsigned)idx + 1;
            return idx;
        }
    }
    int idx = (int)(u->next % (unsigned)n); /* 전부 down이면 그냥 다음 차례 */
    u->next++;
    return idx;
}

void router_mark_down(upstream *u, int backend_index, time_t now) {
    u->backends[backend_index].down_until = now + ROUTER_DOWN_SECS;
}
