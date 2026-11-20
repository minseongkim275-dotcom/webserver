#include "config.h"
#include "test.h"

#include <stdlib.h>
#include <unistd.h>

static int apply(config *c, const char *line) {
    char err[256];
    return config_apply_line(c, line, err, sizeof err);
}

static void defaults(void) {
    config c;
    config_defaults(&c);
    CHECK_INT(c.port, 8080);
    CHECK_STR(c.root, "www");
    CHECK_INT(c.max_body, 1024 * 1024);
    CHECK_INT(c.client_timeout, 15);
    CHECK_INT(c.upstream_timeout, 5);
}

static void simple_directives(void) {
    config c;
    config_defaults(&c);
    CHECK_INT(apply(&c, "listen 9090"), 0);
    CHECK_INT(apply(&c, "root /srv/www   # 주석"), 0);
    CHECK_INT(apply(&c, "max_body 2048"), 0);
    CHECK_INT(apply(&c, "client_timeout 30"), 0);
    CHECK_INT(apply(&c, "upstream_timeout 2"), 0);
    CHECK_INT(apply(&c, "max_conns 500"), 0);
    CHECK_INT(apply(&c, ""), 0);
    CHECK_INT(apply(&c, "   # 주석만"), 0);
    CHECK_INT(c.port, 9090);
    CHECK_STR(c.root, "/srv/www");
    CHECK_INT(c.max_body, 2048);
    CHECK_INT(c.client_timeout, 30);
    CHECK_INT(c.upstream_timeout, 2);
    CHECK_INT(c.max_conns, 500);
}

static void bad_directives(void) {
    config c;
    config_defaults(&c);
    CHECK_INT(apply(&c, "listen abc"), -1);
    CHECK_INT(apply(&c, "listen 70000"), -1);
    CHECK_INT(apply(&c, "listen -1"), -1);
    CHECK_INT(apply(&c, "listen"), -1);
    CHECK_INT(apply(&c, "listen 80 90"), -1);
    CHECK_INT(apply(&c, "max_body 0"), -1);
    CHECK_INT(apply(&c, "client_timeout 12x"), -1);
    CHECK_INT(apply(&c, "bogus 1"), -1);
    CHECK_INT(apply(&c, "root"), -1);
    CHECK_INT(c.port, 8080); /* 실패한 줄은 값을 바꾸지 않는다 */
}

static void backend_addresses(void) {
    backend b;
    CHECK_INT(config_parse_backend("127.0.0.1:9001", &b), 0);
    CHECK_STR(b.label, "127.0.0.1:9001");
    CHECK_INT(ntohs(b.addr.sin_port), 9001);
    CHECK_INT(config_parse_backend("127.0.0.1", &b), -1);
    CHECK_INT(config_parse_backend(":80", &b), -1);
    CHECK_INT(config_parse_backend("localhost:80", &b), -1); /* 이름은 받지 않는다 */
    CHECK_INT(config_parse_backend("1.2.3.4:0", &b), -1);
    CHECK_INT(config_parse_backend("1.2.3.4:65536", &b), -1);
    CHECK_INT(config_parse_backend("1.2.3.4:80x", &b), -1);
    CHECK_INT(config_parse_backend("999.1.1.1:80", &b), -1);
}

static void upstreams_and_routes(void) {
    config c;
    char err[256];
    config_defaults(&c);
    CHECK_INT(apply(&c, "upstream api 127.0.0.1:9001 127.0.0.1:9002"), 0);
    CHECK_INT(apply(&c, "upstream files 10.0.0.5:80"), 0);
    CHECK_INT(apply(&c, "route /api api"), 0);
    CHECK_INT(apply(&c, "route /files/ files"), 0);
    CHECK_INT(apply(&c, "route /public static"), 0);
    CHECK_INT(config_finish(&c, err, sizeof err), 0);
    CHECK_INT(c.nups, 2);
    CHECK_INT(c.ups[0].nbackends, 2);
    CHECK_INT(c.nroutes, 3);
    CHECK_INT(c.routes[0].upstream, 0);
    CHECK_INT(c.routes[1].upstream, 1);
    CHECK_INT(c.routes[2].upstream, -1);

    CHECK_INT(apply(&c, "upstream api 1.1.1.1:1"), -1);    /* 이름 중복 */
    CHECK_INT(apply(&c, "upstream static 1.1.1.1:1"), -1); /* 예약어 */
    CHECK_INT(apply(&c, "upstream x"), -1);
    CHECK_INT(apply(&c, "upstream x notanaddr"), -1);
    CHECK_INT(apply(&c, "route api api"), -1);              /* '/'로 시작해야 함 */
    CHECK_INT(apply(&c, "route /x"), -1);
}

static void unknown_upstream_is_an_error(void) {
    config c;
    char err[256];
    config_defaults(&c);
    CHECK_INT(apply(&c, "route /api ghost"), 0);
    CHECK_INT(config_finish(&c, err, sizeof err), -1);
    CHECK_HAS(err, "ghost");
}

static void file_and_cli_override(void) {
    char path[] = "/tmp/ws_conf_XXXXXX";
    int fd = mkstemp(path);
    const char *text = "listen 7000\nroot /from/file\nupstream api 127.0.0.1:9001\nroute /api api\n";
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);

    config c;
    char err[256];
    char *argv[] = {"webserver", "-c", path, "-p", "7100", "-q", NULL};
    config_defaults(&c);
    CHECK_INT(config_from_args(&c, 6, argv, err, sizeof err), 0);
    CHECK_INT(c.port, 7100);            /* CLI가 파일을 덮어쓴다 */
    CHECK_STR(c.root, "/from/file");
    CHECK_INT(c.quiet, 1);
    CHECK_INT(c.nroutes, 1);

    char *argv2[] = {"webserver", "-r", "/cli/root", "-c", path, NULL};
    config_defaults(&c);
    CHECK_INT(config_from_args(&c, 5, argv2, err, sizeof err), 0);
    CHECK_STR(c.root, "/cli/root");     /* 옵션 순서와 상관없이 CLI가 이긴다 */
    unlink(path);
}

static void cli_errors(void) {
    config c;
    char err[256];
    char *help[] = {"webserver", "-h", NULL};
    char *badport[] = {"webserver", "-p", "99999", NULL};
    char *noval[] = {"webserver", "-p", NULL};
    char *unknown[] = {"webserver", "--wat", NULL};
    char *nofile[] = {"webserver", "-c", "/nonexistent/x.conf", NULL};
    config_defaults(&c);
    CHECK_INT(config_from_args(&c, 2, help, err, sizeof err), 1);
    CHECK_INT(config_from_args(&c, 3, badport, err, sizeof err), -1);
    CHECK_INT(config_from_args(&c, 2, noval, err, sizeof err), -1);
    CHECK_INT(config_from_args(&c, 2, unknown, err, sizeof err), -1);
    CHECK_INT(config_from_args(&c, 3, nofile, err, sizeof err), -1);
    CHECK_HAS(err, "x.conf");
}

int main(void) {
    RUN(defaults);
    RUN(simple_directives);
    RUN(bad_directives);
    RUN(backend_addresses);
    RUN(upstreams_and_routes);
    RUN(unknown_upstream_is_an_error);
    RUN(file_and_cli_override);
    RUN(cli_errors);
    TEST_DONE();
}
