/* fsutil: URL 경로를 root 안의 파일로 바꾼다. 소켓은 만지지 않는다.
 * 파일을 직접 열어서 fd를 돌려준다 (stat과 open 사이에 파일이 바뀌는 틈을 없애려고). */
#ifndef WS_FSUTIL_H
#define WS_FSUTIL_H

#include <stddef.h>
#include <sys/types.h>

/* %XX를 풀어 out에 쓴다. 잘못된 %, %00, out보다 길면 -1 */
int url_decode_path(const char *in, char *out, size_t outsz);

typedef struct {
    int status;           /* 200, 301, 400, 403, 404, 414, 500 */
    int fd;               /* status가 200일 때만 열린 파일 (호출한 쪽이 닫는다), 아니면 -1 */
    off_t size;           /* 파일 크기 */
    char path[4096];      /* 200일 때 실제 파일 경로 (MIME 판정용) */
} static_result;

/* root_real은 realpath로 정규화한 root 디렉터리. url_path는 디코딩 전 경로('?' 앞).
 * - '/'로 끝나면 그 디렉터리의 index.html
 * - 디렉터리인데 '/'로 끝나지 않으면 301 (Location은 url_path + "/")
 * - '.'으로 시작하는 경로 조각(.., ., .git 등)은 403
 * - 심볼릭 링크를 따라가서 root 밖이면 403 */
void static_resolve(const char *root_real, const char *url_path, static_result *res);

#endif
