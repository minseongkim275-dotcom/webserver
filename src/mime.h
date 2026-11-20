/* mime: 파일 경로의 확장자로 Content-Type을 정한다. 모르는 확장자는 application/octet-stream. */
#ifndef WS_MIME_H
#define WS_MIME_H

const char *mime_type_for_path(const char *path);

#endif
