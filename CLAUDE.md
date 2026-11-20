# webserver

크래프톤 정글 13기 프로젝트. C와 POSIX 소켓만으로 만든 epoll 기반 HTTP/1.1 서버 + Reverse Proxy.

## 빌드와 테스트
- 환경: Linux 또는 WSL(Ubuntu-22.04)의 gcc·make. epoll을 쓰므로 macOS에서는 빌드되지 않는다.
- 전체 검사: `bash .claude/check.sh` (작업을 마치기 전에 반드시 통과해야 함. Stop 훅과 CI가 같은 파일을 실행함. 경고는 오류로 다룸)
- 빌드: `make` → `build/webserver`, `build/demo-backend`
- 단위 테스트만: `make unit`
- 게이트 밖: `make asan` (AddressSanitizer + UBSan으로 같은 테스트. CI에서는 실행함)
- 게이트 밖: `make valgrind` (기능 테스트를 valgrind 아래에서 실행. 서버 종료 코드로 누수를 확인함. valgrind 필요)
- 게이트 밖: `make load` (부하 테스트. 의존성 없는 `build/loadgen`으로 정적 파일과 프록시를 동시 연결 수별로 측정해 출력함. 저장소가 WSL의 `/mnt/c`에 있어도 정적 root는 임시 폴더로 복사해 씀)

## 문서
- 과제 요구사항 원문: `docs/requirements.md` (대화에 다시 붙여넣지 않고 이 파일을 읽는다)
- 설계와 그 이유: `docs/design.md`
- 고치지 않기로 한 지적: `docs/known-limits.md` (리뷰의 "선택" 항목을 한 줄씩 기록)
- 하네스 설명: `.claude/HARNESS.md`

## 디렉터리
- `src/`: 서버 본체. `server.c`만 소켓·epoll을 만지고, 나머지 모듈은 순수 함수
- `backend/`: Reverse Proxy 뒤에 두는 데모 백엔드(동적 요청 처리)
- `tests/`: 단위 테스트(`test_*.c`), 기능 테스트(`functional.sh`), 부하 테스트(`load.sh`)
- `www/`: 정적 파일 예시 root
- `conf/`: 설정 파일 예시

## 도메인 규칙
코드만 읽어서는 알 수 없는 이 프로젝트의 약속을 적는다. 규칙마다 그것을 검사하는 테스트를 적는다.
"검사 없음"은 리뷰어가 직접 봐야 하는 항목이고, 테스트로 옮길 후보다.

| 규칙 | 검사하는 곳 |
|---|---|
| 소켓·epoll 호출은 `server.c`에만 둔다. 파싱·라우팅·헤더 고쳐 쓰기는 순수 함수로 둔다 | 검사 없음 |
| 연결은 epoll_wait 묶음 처리가 끝난 뒤에 해제한다 (같은 묶음의 짝 연결 이벤트가 해제된 포인터를 읽지 않게) | 검사 없음 (`make asan` 기능 테스트가 간접 확인) |
