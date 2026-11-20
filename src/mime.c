#include "mime.h"

#include <string.h>
#include <strings.h>

static const struct {
    const char *ext;
    const char *type;
} table[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"xml", "application/xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"pdf", "application/pdf"},
    {"wasm", "application/wasm"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"mp4", "video/mp4"},
    {"mp3", "audio/mpeg"},
};

const char *mime_type_for_path(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    const char *dot = strrchr(name, '.');
    /* 확장자가 없거나, 점으로 시작하는 숨김 파일이거나, 점으로 끝나면 모르는 타입 */
    if (dot && dot != name && dot[1] != '\0') {
        for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
            if (strcasecmp(dot + 1, table[i].ext) == 0) return table[i].type;
    }
    return "application/octet-stream";
}
