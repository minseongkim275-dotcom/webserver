# 구현 계획

설계는 [design.md](design.md), 요구사항은 [requirements.md](requirements.md). 이 문서는 남은 구현을 브랜치 단위로 나눈 작업 목록이다.
브랜치마다 `.claude/skills/work-on-issue` 절차를 따른다: 실패하는 테스트 먼저 → 구현 → `bash .claude/check.sh` → `reviewer` 서브에이전트 → 커밋 → `dev`에 `--no-ff` 병합.

## 진행 상태

| 브랜치 | 상태 |
|---|---|
| `feat/build-setup` | 완료, dev에 병합됨 (Makefile, test.h, buf, check.sh, CI) |
| `feat/http-parser` | 완료, dev에 병합됨 (리뷰 2회) |
| `feat/static-server` | 완료, dev에 병합됨 (fsutil, config, event, server, main, 기능 테스트). 프록시 때 server.c에 이어서 붙인다 |
| `feat/reverse-proxy` | 완료, dev에 병합됨 (router, proxy, server.c 프록시 상태 머신, 데모 백엔드, 기능 테스트. 리뷰 2회) |
| `test/load-and-asan` | 시작 전 |
| README | 시작 전. 팀 규칙대로 목차와 한 줄 요약을 개발자에게 먼저 확인받는다 |

### `feat/http-parser` 1차 리뷰 결과 (반영 전)

꼭 고칠 것:
1. 요청 라인이 정확히 8192바이트이고 끝의 `\r`까지만 도착하면 414가 난다(`src/http.c`의 `!line_end && avail > HTTP_MAX_REQUEST_LINE`). CRLF가 없을 때는 `> HTTP_MAX_REQUEST_LINE + 1`로 비교하고, `\r` 앞뒤로 나눠 넘기는 경계 테스트를 추가한다.
2. `Connection` 헤더가 여러 줄이면 첫 줄만 본다. `Connection: keep-alive`와 `Connection: close`가 같이 오면 닫아야 한다. 모든 Connection 줄의 토큰을 모아 판정하고 테스트를 추가한다.

선택(`docs/known-limits.md`에 한 줄씩 기록): 응답 머리 파서에 제어 문자 검사가 없고 TE+CL이 함께 오는 경우를 처리하지 않음(proxy에서 502 또는 chunked 우선으로 정할 것), 파싱 실패 뒤 `req`의 포인터가 해제된 `raw`를 가리킴(오류 경로에서 비우기), `OPTIONS *`이 400, 헤더 64개·요청 라인 8192바이트 경계 테스트 없음, absolute-form의 호스트가 Host 헤더보다 우선하지 않음.

고친 뒤 리뷰를 한 번 더 받는다(같은 변경의 리뷰는 두 번까지).

`stage/`는 커밋하지 않는 임시 폴더다. 파일을 `src/`로 옮기면서 테스트를 붙이고, 다 옮기면 지운다.

## 공통 약속

- 빌드·테스트는 WSL(Ubuntu-22.04)에서: `bash .claude/check.sh` 또는 `MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu-22.04 --cd "$(pwd -W)" -- make unit`
- `src/*.c`(main.c 제외)는 Makefile이 자동으로 `libws.a`에 넣는다. 테스트는 `tests/test_*.c` 하나가 실행 파일 하나.
- 소켓·epoll 호출은 `server.c`에만. 나머지는 순수 함수 + 단위 테스트.
- 커밋 메시지에 AI 표시를 넣지 않는다 (팀 규칙).

## 브랜치 3: `feat/static-server`

### fsutil (`stage/src/fsutil.[ch]` 초안 있음)
- `url_decode_path(in, out, outsz)`: `%XX` 풀기. 잘못된 `%`, `%00`, 넘침 → -1
- `static_resolve(root_real, url_path, &res)`: `res.status` 200/301/400/403/404/414/500, 200이면 `res.fd`(열린 파일), `res.size`, `res.path`
  - `/`로 끝나면 `index.html`, 디렉터리인데 `/` 없음 → 301, `.`으로 시작하는 경로 조각 → 403, `realpath` 결과가 root 밖 → 403, 일반 파일이 아님 → 403
- 테스트(`tests/test_fsutil.c`): `mkdtemp`로 임시 root를 만들고 파일·디렉터리·`chmod 000` 파일·root 밖을 가리키는 심볼릭 링크를 둔다. root 권한으로 돌면 chmod 테스트는 건너뛴다(`geteuid()==0`).

### config (`stage/src/config.[ch]` 초안 있음)
- 지시어: `listen`, `root`, `max_body`, `client_timeout`, `upstream_timeout`, `max_conns`, `upstream <이름> <ip:port>...`, `route <접두사> <upstream 이름|static>`
- `config_from_args`: `-c 파일`을 먼저 읽고 `-p`, `-r`, `-q`로 덮어쓴 뒤 `config_finish`(route 이름 → 번호). `-h`는 1 반환.
- 테스트(`tests/test_config.c`): 각 지시어, 주석·빈 줄, 잘못된 숫자·주소·모르는 지시어 오류, 정의 안 된 upstream을 가리키는 route 오류, CLI가 파일 값을 덮어씀.

### event (`src/event.[ch]`, 새로)
- epoll 래퍼: `ev_init`, `ev_add(fd, events, ptr)`, `ev_mod`, `ev_del`, `ev_wait(out, max, timeout_ms)`. 이벤트 플래그는 자체 상수(`EV_READ`, `EV_WRITE`, `EV_HUP|ERR`). macOS 대비 경계일 뿐 구현은 epoll 하나.

### server (`src/server.[ch]`, 새로)
- 연결 구조체: `fd`, `kind`(LISTEN/CLIENT/UPSTREAM), `state`, `buf in, out`, `http_request req`, 파일 전송용 `file_fd/file_off/file_end`, `keep_alive`, `last_active`, `peer`(클라이언트↔백엔드), 클라이언트 IP 문자열, 연결 목록용 `prev/next`, `closed` 플래그.
- 상태: `READ_HEAD → READ_BODY → WRITE`(정적) / `UPSTREAM_CONNECT → UPSTREAM_SEND → RELAY`(프록시). keep-alive면 응답을 다 보낸 뒤 `in`에 남은 바이트로 다음 요청을 이어서 파싱(파이프라이닝).
- level-triggered. `out`이 비어 있지 않을 때만 EPOLLOUT 켬. `send(..., MSG_NOSIGNAL)`, EAGAIN/EINTR/부분 전송 처리.
- 정적 파일: `out`이 비면 64KB씩 `pread`해서 채운다. HEAD는 머리만.
- 본문: `Content-Length > max_body` → 413 후 닫기. `Expect: 100-continue`이고 본문이 아직 없으면 `HTTP/1.1 100 Continue\r\n\r\n`을 먼저 보냄.
- 정적 경로에 GET/HEAD 외 메서드 → 405 (`Allow: GET, HEAD`).
- 오류 응답(4xx/5xx) 뒤에는 닫는다.
- **지연 해제**: `conn_close()`는 fd를 닫고 epoll에서 빼고 `closed=1`로 표시해 graveyard 목록에 넣기만 한다. `epoll_wait` 결과를 다 처리한 뒤 graveyard를 `free`. 이벤트 처리 시작에서 `closed`면 건너뜀.
- 타임아웃: `epoll_wait` 타임아웃 1000ms, 매 반복마다 연결 목록을 훑어 `client_timeout`(클라이언트 조용함) / `upstream_timeout`(백엔드 무응답 → 504) 처리.
- accept: listen 소켓 non-blocking, EAGAIN까지 반복. `max_conns` 도달 또는 EMFILE/ENFILE이면 listen 소켓을 epoll에서 1초 뺐다가 다시 넣음.
- 접근 로그(`-q`면 끔): `IP "METHOD target HTTP/x.y" status bytes ms [upstream=ip:port]`
- 셧다운: SIGINT/SIGTERM 핸들러(`sigaction`, SA_RESTART 없음)는 `volatile sig_atomic_t` 플래그만 세움. 루프가 보면 listen 소켓을 닫고, 응답 중인 연결은 최대 5초 기다린 뒤 전부 닫고 해제, 종료 코드 0. SIGPIPE는 `SIG_IGN`.
- listen 소켓: `SO_REUSEADDR`, `bind` 실패 시 `strerror`와 함께 종료 코드 1.

### main (`src/main.c`, 새로)
- `config_defaults` → `config_from_args` → `realpath(root)` 실패 시 오류 → 시그널 설정 → `server_run(&cfg)`.
- 사용법: `webserver [-c conf] [-p port] [-r root] [-q]`

### 예시 파일
- `www/index.html`, `www/style.css`, `www/app.js`, `www/img/logo.png`(작은 PNG), `conf/webserver.conf`(design.md "설정" 예시)

### 기능 테스트 (`tests/functional.sh`, Makefile `test` 타깃에 추가)
- `mktemp -d`로 root를 만들고 서버를 높은 포트로 띄운 뒤 curl과 bash `/dev/tcp`로 확인, `trap`으로 서버 종료.
- 확인할 것: 200 + Content-Type(html/css/png), HEAD(본문 없음), 404, 403(`..`, `%2e%2e`, 숨김 파일, root 밖 심볼릭 링크, chmod 000 — root 사용자면 건너뜀), 301(슬래시 없는 디렉터리), 405(정적에 POST), 400(깨진 요청 라인), 414, 431, 501, 505, 413, keep-alive로 요청 두 개(`curl` 같은 연결 재사용 `-v`의 "Re-using"), 파이프라이닝(raw로 요청 두 개를 한 번에), 클라이언트가 머리 중간에 끊기 → 서버가 살아 있음, 동시 50개 요청(`xargs -P`), SIGTERM → 종료 코드 0.

## 브랜치 4: `feat/reverse-proxy`

### router (`src/router.[ch]`)
- `router_match(cfg, path)`: 가장 긴 접두사. 경계 규칙: 접두사가 `/`로 끝나거나, path가 접두사에서 끝나거나, 다음 글자가 `/`. (`/api`는 `/api`, `/api/x`와 맞고 `/apix`와는 안 맞음.) upstream 번호 또는 -1(정적).
- `router_pick(upstream *u, time_t now)`: `down_until > now`인 백엔드는 건너뛰는 Round-Robin. 전부 down이면 그냥 다음 차례. `router_mark_down(u, i, now)`: 10초.
- 테스트: 접두사 경계, 최장 일치, 순환 순서, down 건너뛰기.

### proxy (`src/proxy.[ch]`)
- `proxy_build_request(req, client_ip, fallback_host, out)`: `METHOD path[?query] HTTP/1.1`, 홉 단위 헤더(Connection, Keep-Alive, Proxy-Connection, TE, Trailer, Upgrade, Expect, 그리고 Connection 값에 적힌 이름) 제거, `X-Forwarded-For`(있으면 `, ip` 덧붙임), `X-Forwarded-Proto: http`, Host 없으면 `fallback_host`, 마지막에 `Connection: close`. 본문은 server가 뒤에 붙임.
- `proxy_rewrite_response_head(head, len, keep_alive, out)`: Connection/Keep-Alive/Proxy-Connection 제거, `Connection: keep-alive|close` 추가.
- 클라이언트 keep-alive 유지 조건: 클라이언트가 원하고 + (Content-Length가 있거나, HEAD 요청이거나, 204/304). chunked나 길이 없음 → 응답 뒤 닫음. 백엔드 1xx(101 제외)는 건너뛰고 다음 머리를 파싱.
- 테스트: 헤더 제거·추가, XFF 덧붙이기, absolute-form이 origin-form으로 바뀜, 응답 머리 고쳐 쓰기.

### server 쪽
- non-blocking `connect` → EINPROGRESS → EPOLLOUT에서 `SO_ERROR` 확인. 실패하면 `router_mark_down` 후 다음 백엔드(백엔드 수만큼까지). 전부 실패 → 502.
- 요청을 보낸 뒤에는 재시도하지 않음. 백엔드가 머리 전에 끊김·머리가 깨짐 → 502. `upstream_timeout` 동안 응답 없음 → 504 (이미 응답을 보내기 시작했으면 그냥 둘 다 닫음).
- 클라이언트 `out`이 256KB를 넘으면 백엔드 EPOLLIN을 끄고, 줄어들면 다시 켬.
- 클라이언트가 끊기면 짝 백엔드 연결도 닫음 (지연 해제 목록으로).

### 데모 백엔드 (`backend/backend.c`, Makefile에 `build/demo-backend` 추가)
- `backend -p 9001 -n 이름`. 연결마다 detached pthread, `http_parse_request`로 요청을 읽고 `Connection: close`로 응답.
- `GET /api/hello` → JSON `{"backend":"이름","count":N}` (N은 원자적 카운터)
- `POST /api/echo` → 받은 본문 그대로
- `GET /api/sum?a=1&b=2` → `{"sum":3}` (동적 처리 예시)
- `GET /api/slow?ms=N` → N밀리초 뒤 응답 (504 테스트)
- `GET /api/chunked` → chunked 응답 (프록시가 그대로 전달하고 닫는지)
- `GET /api/big?kb=N` → N KB 본문 (역압 테스트)
- `GET /api/drop` → 응답 없이 닫음 (502 테스트)
- 그 외 → 404 JSON

### 기능 테스트 추가
- 백엔드 두 개 + 서버. `/api/hello` 네 번 → 이름이 번갈아 나옴. POST echo, sum, chunked, big(바이트 수 일치), slow(504), drop(502), 백엔드 하나를 죽인 뒤에도 200(다른 쪽으로 재시도), 둘 다 죽이면 502, 프록시 중 클라이언트가 끊겨도 서버가 살아 있음.

## 브랜치 5: `test/load-and-asan`

- `tests/load.sh`: 서버(+백엔드)를 `-q`로 띄우고 `ab -n 10000 -c {1,10,100,1000} -k`로 정적과 프록시를 측정해 표로 출력. `ab`가 없으면 설치 방법을 알려 주고 종료 코드 0. 결과는 손으로 문서에 옮기지 않고 명령 출력으로 남긴다(팀 규칙).
- `make asan`이 기능 테스트까지 돌게 확인(`detect_leaks=1`). `valgrind --leak-check=full`로 서버를 띄워 기능 테스트를 한 번 돌리는 `make valgrind` 타깃(게이트 밖).
- CLAUDE.md "게이트 밖" 줄이 실제 명령과 맞는지 확인.

## 마지막

- README: 목차와 한 줄 요약을 개발자에게 먼저 보여 주고 확인받은 뒤 쓴다.
- 서버를 실제로 띄워 curl 출력(정적, 프록시 Round-Robin, 오류 응답)을 개발자에게 보여 준다.
- `dev → main` PR 본문 초안(`바꾼 것 / 검사 결과 / 남은 경고`)을 만들어 보여 준다. 직접 올리지 않는다.
