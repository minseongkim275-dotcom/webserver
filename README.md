# webserver

C와 POSIX 소켓만으로 만든 epoll 기반 HTTP/1.1 웹 서버와 Reverse Proxy. 외부 HTTP·이벤트 루프 라이브러리를 쓰지 않습니다. (크래프톤 정글 13기 과제)

## 목차

1. [빠른 시작](#빠른-시작)
2. [기능](#기능)
3. [설정과 CLI](#설정과-cli)
4. [구조](#구조)
5. [설계 결정](#설계-결정)
6. [HTTP 규칙](#http-규칙)
7. [테스트](#테스트)
8. [부하 테스트](#부하-테스트)
9. [알려진 한계](#알려진-한계)

## 빠른 시작

Linux 또는 WSL(Ubuntu)에서 gcc와 make만 있으면 됩니다. epoll을 쓰므로 macOS에서는 빌드되지 않습니다.

```sh
make                     # build/webserver, build/demo-backend, build/loadgen
build/demo-backend -p 9001 -n a &
build/demo-backend -p 9002 -n b &
build/webserver -c conf/webserver.conf
```

```sh
curl -i localhost:8080/                 # 정적 파일 (www/index.html)
curl localhost:8080/api/hello           # 백엔드 a, b로 번갈아 전달 (Round-Robin)
curl -d 'name=jungle' localhost:8080/api/echo
curl 'localhost:8080/api/sum?a=2&b=40'
```

종료는 `Ctrl-C`(SIGINT) 또는 SIGTERM입니다. 진행 중인 응답을 최대 5초 기다린 뒤 정리하고 종료 코드 0으로 끝납니다.

## 기능

- **정적 파일**: root 아래 파일을 서빙합니다. MIME 매핑, `index.html`, 슬래시 없는 디렉터리는 301, `..`·숨김 파일·root 밖 심볼릭 링크는 403, 없으면 404.
- **HTTP/1.1**: keep-alive, 파이프라이닝, `Expect: 100-continue`, `HEAD`, `Content-Length` 기반 요청 본문.
- **Reverse Proxy**: 경로 접두사로 upstream(백엔드 1..N)을 고르고, upstream 안에서 Round-Robin으로 나눕니다.
  - 연결에 실패하면 다음 백엔드로 같은 요청을 다시 보내고(요청을 보내기 전에만), 모두 실패하면 502, 응답이 `upstream_timeout` 안에 오지 않으면 504입니다.
  - 연결에 실패한 백엔드는 10초 동안 건너뜁니다.
  - 홉 단위 헤더를 빼고 `X-Forwarded-For`, `X-Forwarded-Proto`를 붙입니다. 경로는 접두사를 떼지 않고 그대로 전달합니다.
  - 클라이언트가 느려 출력이 256KB 쌓이면 백엔드에서 읽기를 멈춥니다(역압).
- **동적 처리**: 위 프록시로 데모 백엔드(`backend/backend.c`)에 넘깁니다. `hello`, `echo`, `sum`, `slow`, `chunked`, `big`, `headers`, `drop` 엔드포인트가 있습니다.
- **견고함**: 잘못된 요청, 긴 URI·헤더, 큰 본문, 백엔드 무응답, 클라이언트 중간 끊김을 처리합니다. 유휴 연결은 `client_timeout` 뒤에 닫습니다.

## 설정과 CLI

```sh
build/webserver [-c conf] [-p port] [-r root] [-q]
```

| 옵션 | 뜻 |
|---|---|
| `-c conf` | 설정 파일 |
| `-p port` | 포트 (설정 파일보다 우선) |
| `-r root` | 정적 파일 폴더 (설정 파일보다 우선) |
| `-q` | 접근 로그 끔 |

설정 파일 예시는 [conf/webserver.conf](conf/webserver.conf)입니다.

| 지시어 | 기본값 | 뜻 |
|---|---|---|
| `listen <port>` | 8080 | 포트 |
| `root <dir>` | `www` | 정적 파일 root |
| `max_body <bytes>` | 1048576 | 요청 본문 한도. 넘으면 413 |
| `client_timeout <초>` | 15 | 조용한 클라이언트 연결을 닫기까지 |
| `upstream_timeout <초>` | 5 | 백엔드 응답을 기다리는 시간. 넘으면 504 |
| `max_conns <n>` | 10000 | 동시 연결 한도 |
| `upstream <이름> <ip:port>...` | | 백엔드 묶음. 이름이 아니라 IP만 받습니다(이름 풀이는 블로킹이라 이벤트 루프에서 하지 않음) |
| `route <접두사> <upstream 이름\|static>` | | 가장 긴 접두사가 이깁니다. 맞는 route가 없으면 정적 파일 |

접근 로그는 stdout에 한 줄씩 나옵니다: `IP "METHOD target HTTP/x.y" 상태 바이트 ms [upstream=ip:port]`.

## 구조

```
클라이언트 ──▶ webserver (epoll 루프 1개)
                 ├─ router: 가장 긴 경로 접두사 일치
                 ├─ 정적: root 아래 파일 → MIME → 200/301/403/404/500
                 └─ 프록시: upstream에서 Round-Robin → demo-backend (1..N)
```

| 모듈 | 하는 일 | 소켓을 만지는가 |
|---|---|---|
| `src/buf` | 늘어나는 바이트 버퍼 | 아니오 |
| `src/http` | 요청·응답 머리 파싱, 상태 문구, 응답 머리 쓰기 | 아니오 |
| `src/mime` | 확장자 → Content-Type | 아니오 |
| `src/fsutil` | URL 경로 → root 안의 파일, 상태 코드 | 아니오 |
| `src/config` | 설정 파일·CLI | 아니오 |
| `src/router` | 경로 → upstream, Round-Robin | 아니오 |
| `src/proxy` | 백엔드 요청 머리 만들기, 응답 머리 고쳐 쓰기 | 아니오 |
| `src/event` | epoll 래퍼 (macOS 지원을 넣을 때 바꿀 경계) | 예 |
| `src/server` | 이벤트 루프, 연결 상태 머신, 타임아웃, 종료 | 예 |

소켓을 만지지 않는 모듈은 전부 순수 함수라서 단위 테스트로 검사합니다. 연결은 클라이언트 쪽 `READ_HEAD → READ_BODY → WRITE` 또는 `PROXY`로 움직이고, 백엔드 쪽은 `CONNECT → SEND → HEAD → RELAY`로 움직입니다. 자세한 그림은 [docs/design.md](docs/design.md)에 있습니다.

## 설계 결정

| 후보 | 장점 | 단점 | 결정 |
|---|---|---|---|
| epoll 이벤트 루프 (단일 스레드, non-blocking) | 연결 수가 스레드 수에 묶이지 않음(C10K) | 연결마다 상태 머신이 필요해 복잡함, Linux 전용 | **선택** |
| 스레드 풀 + blocking I/O | 코드가 위에서 아래로 읽힘, 이식성 | keep-alive 연결이 워커를 점유함 | 제외 |
| fork per request | 가장 단순 | 프로세스끼리 Round-Robin 상태를 공유하기 어려움 | 제외 |

이벤트 루프에서 특히 신경 쓴 것:

- **지연 해제**: 연결을 닫아도 구조체는 한 번의 `epoll_wait` 결과를 다 처리한 뒤에 해제합니다. 클라이언트와 백엔드 연결이 짝으로 닫히는데, 같은 묶음에 남은 이벤트가 해제된 포인터를 읽지 않게 하려는 것입니다.
- **linger close**: 오류 응답 뒤에 바로 `close`하면 아직 오는 요청 본문 때문에 RST가 나가 클라이언트가 응답을 못 읽을 수 있어서, 쓰기 쪽만 닫고 잠시 나머지를 버립니다.
- **재시도 범위**: 연결 단계에서만 다음 백엔드로 넘어갑니다. 요청을 보낸 뒤에는 POST가 두 번 처리될 수 있어서 재시도하지 않습니다.
- **역압**: 클라이언트가 느리면 백엔드 읽기를 멈춰 메모리가 무한히 늘지 않게 합니다.

## HTTP 규칙

| 상황 | 응답 |
|---|---|
| 요청 라인이 8KB를 넘음 | 414 |
| 머리 전체가 16KB를 넘거나 헤더가 64개를 넘음 | 431 |
| 형식 오류, HTTP/1.1인데 Host 없음, Content-Length가 숫자가 아니거나 서로 다름 | 400 |
| 모르는 메서드, Transfer-Encoding 요청 본문 | 501 |
| 1.0, 1.1이 아닌 버전 | 505 |
| 본문이 `max_body`를 넘음 | 413 |
| 정적 경로에 GET/HEAD 말고 다른 메서드 | 405 |
| `..`·숨김 파일·root 밖 | 403 |
| 백엔드 연결 실패(모든 후보), 백엔드 응답이 깨짐 | 502 |
| 백엔드가 `upstream_timeout` 안에 응답하지 않음 | 504 |

오류 응답(4xx/5xx) 뒤에는 연결을 닫습니다.

## 테스트

| 명령 | 내용 |
|---|---|
| `bash .claude/check.sh` | 전체 검사(`-Werror`). 단위 테스트와 기능 테스트. 작업을 마치기 전 필수이며 CI도 같은 파일을 실행합니다 |
| `make unit` | 단위 테스트만 (`buf`, `http`, `mime`, `fsutil`, `config`, `router`, `proxy`) |
| `make functional` | `tests/functional.sh`: 실제 서버를 띄워 curl과 raw 소켓으로 확인 |
| `make asan` | AddressSanitizer + UBSan + 누수 검사로 같은 테스트 (CI에서도 실행) |
| `make valgrind` | 기능 테스트를 valgrind 아래에서 실행 |

기능 테스트는 정적 파일, 오류 응답, 경로 이탈 차단, keep-alive, 파이프라이닝, 100-continue, 클라이언트 끊김, 동시 요청, 프록시 라우팅·Round-Robin·재시도·504·502, 느린 클라이언트, SIGTERM 종료를 확인합니다. 개수는 문서에 적지 않고 출력(`functional.sh: N passed`)으로 봅니다.

## 부하 테스트

```sh
make load                                     # 동시 연결 1, 10, 100, 1000
DURATION=5 CONNS="10 100" bash tests/load.sh  # 조건을 바꿔서
```

`ab`, `wrk`, `hey`가 없는 환경에서도 돌도록 의존성 없는 부하 생성기(`tests/loadgen.c`)를 함께 두었습니다. 서버, 백엔드, 부하 생성기가 같은 컴퓨터의 CPU를 나눠 쓰므로 절대값이 아니라 추세로 읽어야 합니다. 결과는 문서에 손으로 옮기지 않고 명령 출력으로 확인합니다.

저장소가 WSL의 `/mnt/c`(9p)에 있으면 파일 호출이 느려 서버가 아니라 파일 시스템을 재게 됩니다. 그래서 `load.sh`는 기본으로 `www`를 임시 폴더로 복사해서 씁니다.

## 알려진 한계

고치지 않기로 한 지적은 [docs/known-limits.md](docs/known-limits.md)에 한 줄씩 적습니다. 주요한 것만 옮기면:

- Linux 전용입니다(epoll). `src/event`가 경계라서 kqueue나 poll 구현을 추가하면 macOS에서도 빌드할 수 있습니다.
- 정적 파일의 `open`/`pread`가 이벤트 루프 스레드에서 블로킹으로 실행됩니다. 느린 파일 시스템에서는 프록시 요청까지 같이 느려집니다.
- TLS, HTTP/2, chunked **요청** 본문, Forward Proxy, 캐시는 만들지 않았습니다.
- 백엔드 주소는 IP만 받고, 헬스 체크는 연결 실패 시 10초 동안 건너뛰는 것이 전부입니다.
- 타임아웃 검사는 1초마다 모든 연결을 훑습니다(O(n)). 연결이 아주 많으면 타이머 힙이나 휠로 바꿔야 합니다.

요구사항 원문은 [docs/requirements.md](docs/requirements.md)에 있습니다.
