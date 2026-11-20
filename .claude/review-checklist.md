# 프로젝트 리뷰 항목 (🟩 특화 — webserver)

- 오류 경로(파싱 실패, 연결 끊김, 백엔드 실패)에서 malloc한 메모리, 연 fd, epoll 등록을 빠짐없이 정리하는가
- 연결을 닫은 뒤 같은 epoll_wait 묶음 안에서 그 연결 포인터를 다시 쓰지 않는가 (지연 해제 목록을 거치는가)
- 외부 입력 길이(요청 라인, 헤더, 본문, 백엔드 응답 머리)를 버퍼에 쓰기 전에 한도와 비교하는가
- non-blocking 소켓의 read/write/connect가 EAGAIN·EINTR·부분 전송을 모두 다루는가
- 정적 파일 경로가 root 밖(.., 심볼릭 링크)으로 나갈 수 없는가
- 상태 코드와 Connection 헤더가 docs/design.md의 "HTTP 규칙" 표와 맞는가
