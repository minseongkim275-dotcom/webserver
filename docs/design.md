# 설계

요구사항 원문은 [requirements.md](requirements.md). 이 문서는 "어떻게 만들었는가"와 "왜 그렇게 정했는가"를 적는다.

## 목표와 범위

- 외부 라이브러리 없이 C + POSIX 소켓만으로 HTTP/1.1 서버를 만든다.
- 정적 파일 서빙과 Reverse Proxy(경로 라우팅 + Round-Robin)를 한 프로세스가 한다.
- 동적 요청은 직접 짠 C 백엔드(`backend/`)로 전달해 처리한다.
- 하지 않는 것: TLS, HTTP/2, chunked 요청 본문, Forward Proxy, 캐시, macOS 빌드(epoll은 Linux 전용).

## 고른 방식

| 후보 | 장점 | 단점 | 결정 |
|---|---|---|---|
| A. epoll 이벤트 루프 (단일 스레드, non-blocking) | 연결 수가 스레드 수에 묶이지 않음(C10K), 중점 포인트를 정면으로 다룸 | 연결마다 상태 머신이 필요해 복잡함, Linux 전용 | **선택** |
| B. 스레드 풀 + blocking I/O | 코드가 위에서 아래로 읽혀 쉬움, 이식성 | keep-alive 연결이 워커를 점유함 | 보류 |
| C. fork per request | 가장 단순 | 프로세스끼리 Round-Robin 상태 공유가 어려움 | 제외 |

## 구조

```
클라이언트 ──▶ webserver (epoll 루프 1개)
                 ├─ 라우터: 가장 긴 경로 접두사 일치
                 ├─ 정적: root 아래 파일 → MIME → 200/301/403/404/500
                 └─ 프록시: upstream 묶음에서 Round-Robin → backend(1..N)
```

| 모듈 | 하는 일 | 소켓을 만지는가 |
|---|---|---|
| `buf` | 늘어나는 바이트 버퍼 | 아니오 |
| `http` | 요청 머리 파싱, 응답 머리 파싱, 상태 문구, keep-alive 판정 | 아니오 |
| `mime` | 확장자 → Content-Type | 아니오 |
| `fsutil` (정적) | URL 경로를 root 안의 파일로 바꾸고 상태 코드를 정함 | 아니오 |
| `config` | 설정 파일·CLI 인자 읽기 | 아니오 |
| `router` | 경로 → upstream 묶음 선택, Round-Robin | 아니오 |
| `proxy` | 백엔드로 보낼 요청 만들기, 백엔드 응답 머리 고쳐 쓰기 | 아니오 |
| `server` | epoll 루프, 연결 상태 머신, 타임아웃, 셧다운 | 예 |

소켓을 만지지 않는 모듈은 전부 순수 함수로 두고 단위 테스트한다. `server`는 기능 테스트(curl, raw 소켓)로 검사한다.

## 연결 상태 머신

```
READ_HEAD ─(머리 완성)─▶ READ_BODY ─(본문 완성)─▶ 처리
   ▲                                              │
   │                         정적: WRITE (파일은 64KB씩 pread)
   │                         프록시: UPSTREAM_CONNECT → UPSTREAM_SEND → RELAY
   └──────(keep-alive이고 응답을 다 보냄)──────────┘
```

- epoll은 level-triggered로 쓴다. 보낼 데이터가 있을 때만 EPOLLOUT을 켠다.
- 한 번의 `epoll_wait` 결과 안에서 닫힌 연결을 바로 `free`하지 않고 목록에 모았다가 묶음 처리가 끝난 뒤 해제한다. (클라이언트를 닫으면서 짝인 백엔드 연결도 닫았는데, 같은 묶음에 백엔드 이벤트가 남아 있으면 해제된 메모리를 읽게 된다.)
- 응답을 다 보내기 전에는 다음 요청을 파싱하지 않는다. 파이프라이닝된 요청은 버퍼에 남겨 두었다가 응답이 끝나면 이어서 처리한다.

## HTTP 규칙

| 상황 | 응답 |
|---|---|
| 요청 라인이 8KB를 넘음 | 414 |
| 머리 전체가 16KB를 넘거나 헤더가 64개를 넘음 | 431 |
| 형식 오류, HTTP/1.1인데 Host 없음, Content-Length가 숫자가 아니거나 서로 다름 | 400 |
| 모르는 메서드, Transfer-Encoding 요청 본문 | 501 |
| HTTP/1.0, 1.1이 아닌 버전 | 505 |
| 본문이 `max_body`를 넘음 | 413 |
| 정적 경로에 GET/HEAD 말고 다른 메서드 | 405 |
| 경로에 `..` 조각이 있거나 root 밖을 가리킴(심볼릭 링크 포함) | 403 |
| 백엔드 연결 실패(모든 후보), 백엔드 응답이 깨짐 | 502 |
| 백엔드가 `upstream_timeout` 안에 응답하지 않음 | 504 |

- keep-alive: HTTP/1.1은 `Connection: close`가 없으면 유지, HTTP/1.0은 `Connection: keep-alive`가 있어야 유지. 4xx/5xx 오류 응답 뒤에는 닫는다.
- 클라이언트 연결이 `client_timeout` 동안 조용하면 닫는다 (느린 클라이언트가 연결을 붙잡는 것 방지).

## Reverse Proxy

- 백엔드에는 `Connection: close`로 요청하고, 홉 단위 헤더(Connection, Keep-Alive, Proxy-Connection, TE, Trailer, Upgrade)를 뺀다. `X-Forwarded-For`, `X-Forwarded-Proto`를 붙인다.
- 백엔드 응답 머리를 파싱해서 Content-Length가 있으면 클라이언트 연결을 유지하고, 없으면(chunked 또는 EOF로 끝남) 응답 뒤에 닫는다.
- 연결 실패(`connect` 오류)일 때만 다음 백엔드로 다시 시도한다. 요청을 이미 보낸 뒤에는 재시도하지 않는다 (POST가 두 번 처리될 수 있음).
- 클라이언트 쪽 출력 버퍼가 256KB를 넘으면 백엔드에서 읽기를 멈춘다 (느린 클라이언트 때문에 메모리가 무한히 늘지 않게).

## 종료 처리

- `SIGPIPE`는 무시하고, `send`에는 `MSG_NOSIGNAL`을 준다.
- `SIGINT`/`SIGTERM`은 플래그만 세운다. 루프가 플래그를 보면 listen 소켓을 닫고, 진행 중인 응답을 최대 5초 기다린 뒤 모든 연결과 메모리를 정리하고 끝난다.

## 설정

```
# conf/webserver.conf
listen 8080
root www
max_body 1048576
client_timeout 15
upstream_timeout 5
upstream api 127.0.0.1:9001 127.0.0.1:9002
route /api api
```

CLI 인자(`-p`, `-r`, `-c`, `-q`)가 설정 파일 값보다 우선한다. `route`에 걸리지 않은 경로는 정적 파일로 처리한다.

## 구현자가 정한 것

- 이벤트 백엔드를 `event.h` 뒤에 숨겨 두었지만 구현은 epoll 하나뿐이다. macOS 지원은 `kqueue`/`poll` 구현을 추가하면 된다.
- 타임아웃 검사는 1초마다 모든 연결을 훑는다(O(n)). 연결이 아주 많으면 타이머 힙이나 휠로 바꿔야 한다.
- 데모 백엔드는 연결마다 스레드를 하나 만드는 단순한 구조다. 웹 서버 본체가 아니라 테스트 상대역이다.
