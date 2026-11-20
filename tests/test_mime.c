#include "mime.h"
#include "test.h"

static void known_types(void) {
    CHECK_STR(mime_type_for_path("/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for_path("/a/b.HTM"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for_path("style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type_for_path("app.js"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type_for_path("data.json"), "application/json");
    CHECK_STR(mime_type_for_path("logo.png"), "image/png");
    CHECK_STR(mime_type_for_path("photo.JPG"), "image/jpeg");
    CHECK_STR(mime_type_for_path("photo.jpeg"), "image/jpeg");
    CHECK_STR(mime_type_for_path("anim.gif"), "image/gif");
    CHECK_STR(mime_type_for_path("icon.svg"), "image/svg+xml");
    CHECK_STR(mime_type_for_path("favicon.ico"), "image/x-icon");
    CHECK_STR(mime_type_for_path("doc.pdf"), "application/pdf");
    CHECK_STR(mime_type_for_path("notes.txt"), "text/plain; charset=utf-8");
}

static void unknown_or_missing_extension(void) {
    const char *bin = "application/octet-stream";
    CHECK_STR(mime_type_for_path("archive.xyz"), bin);
    CHECK_STR(mime_type_for_path("Makefile"), bin);
    CHECK_STR(mime_type_for_path("/dir.html/file"), bin); /* 점이 디렉터리 이름에만 있음 */
    CHECK_STR(mime_type_for_path("/.hidden"), bin);       /* 점으로 시작하는 파일 이름 */
    CHECK_STR(mime_type_for_path("name."), bin);
    CHECK_STR(mime_type_for_path(""), bin);
}

int main(void) {
    RUN(known_types);
    RUN(unknown_or_missing_extension);
    TEST_DONE();
}
