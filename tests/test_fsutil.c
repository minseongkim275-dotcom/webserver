#include "fsutil.h"
#include "test.h"

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[PATH_MAX];    /* 임시 root (realpath 정규화됨) */
static char outside[PATH_MAX]; /* root 밖의 형제 디렉터리 */

#define PBUF (PATH_MAX * 2 + 64)

static void join(char *out, const char *dir, const char *rel) { snprintf(out, PBUF, "%s/%s", dir, rel); }

static void put(const char *dir, const char *rel, const char *content) {
    char p[PBUF];
    join(p, dir, rel);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static void make_dir(const char *dir, const char *rel) {
    char p[PBUF];
    join(p, dir, rel);
    mkdir(p, 0755);
}

static void link_to(const char *target, const char *dir, const char *rel) {
    char p[PBUF];
    join(p, dir, rel);
    if (symlink(target, p) != 0) T_FAIL_AT("symlink %s failed", p);
}

static void setup(void) {
    char tmpl[] = "/tmp/ws_fsutil_XXXXXX";
    char *base = mkdtemp(tmpl);
    char r[PBUF], o[PBUF], p[PBUF];
    if (!base) {
        T_FAIL_AT("mkdtemp failed");
        return;
    }
    join(r, base, "root");
    join(o, base, "outside");
    mkdir(r, 0755);
    mkdir(o, 0755);
    if (!realpath(r, root) || !realpath(o, outside)) T_FAIL_AT("realpath failed");

    put(root, "index.html", "<h1>home</h1>");
    put(root, "a.txt", "hello");
    put(root, ".secret", "no");
    put(outside, "passwd", "root:x");
    make_dir(root, "sub");
    put(root, "sub/index.html", "sub");
    make_dir(root, "empty");
    put(root, "my file.txt", "space");

    link_to(outside, root, "link_out");
    join(p, root, "a.txt");
    link_to(p, root, "link_file");
    put(root, "noperm.txt", "x");
    join(p, root, "noperm.txt");
    chmod(p, 0000);
}

static int status_of(const char *url) {
    static_result r;
    static_resolve(root, url, &r);
    if (r.fd >= 0) close(r.fd);
    return r.status;
}

static void decode(void) {
    char out[64];
    CHECK_INT(url_decode_path("/a%20b", out, sizeof out), 0);
    CHECK_STR(out, "/a b");
    CHECK_INT(url_decode_path("/%7Euser", out, sizeof out), 0);
    CHECK_STR(out, "/~user");
    CHECK_INT(url_decode_path("/a%2", out, sizeof out), -1);
    CHECK_INT(url_decode_path("/a%zz", out, sizeof out), -1);
    CHECK_INT(url_decode_path("/a%00b", out, sizeof out), -1);
    CHECK_INT(url_decode_path("/aaaaaaaaaa", out, 5), -1); /* 출력 공간 부족 */
}

static void serves_files(void) {
    static_result r;
    static_resolve(root, "/a.txt", &r);
    CHECK_INT(r.status, 200);
    CHECK(r.fd >= 0);
    CHECK_INT(r.size, 5);
    CHECK_HAS(r.path, "/a.txt");
    if (r.fd >= 0) close(r.fd);

    CHECK_INT(status_of("/my%20file.txt"), 200);
    CHECK_INT(status_of("/link_file"), 200); /* root 안을 가리키는 링크는 허용 */
}

static void directories(void) {
    CHECK_INT(status_of("/"), 200);          /* index.html */
    CHECK_INT(status_of("/sub/"), 200);
    CHECK_INT(status_of("/sub"), 301);       /* 슬래시 없음 */
    CHECK_INT(status_of("/empty/"), 404);    /* index.html 없음 */
}

static void not_found(void) {
    CHECK_INT(status_of("/nope.html"), 404);
    CHECK_INT(status_of("/a.txt/extra"), 404);
}

static void forbidden(void) {
    CHECK_INT(status_of("/../outside/passwd"), 403);
    CHECK_INT(status_of("/%2e%2e/outside/passwd"), 403);
    CHECK_INT(status_of("/sub/../a.txt"), 403);   /* '..' 조각은 모두 거절 */
    CHECK_INT(status_of("/./a.txt"), 403);
    CHECK_INT(status_of("/.secret"), 403);        /* 숨김 파일 */
    CHECK_INT(status_of("/link_out/passwd"), 403); /* 링크로 root 밖 */
    if (geteuid() != 0) CHECK_INT(status_of("/noperm.txt"), 403);
}

static void bad_requests(void) {
    CHECK_INT(status_of("a.txt"), 400);       /* '/'로 시작하지 않음 */
    CHECK_INT(status_of("/a%zz"), 400);
    CHECK_INT(status_of("/a%00.txt"), 400);
    char big[PATH_MAX + 64];
    memset(big, 'a', sizeof big);
    big[0] = '/';
    big[sizeof big - 1] = '\0';
    CHECK_INT(status_of(big), 414);
}

int main(void) {
    setup();
    RUN(decode);
    RUN(serves_files);
    RUN(directories);
    RUN(not_found);
    RUN(forbidden);
    RUN(bad_requests);
    TEST_DONE();
}
