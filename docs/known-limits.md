# 고치지 않기로 한 지적

리뷰의 "선택" 항목을 한 줄씩 적는다. 리뷰어는 여기 적힌 항목을 다시 올리지 않는다.

- (http-parser) 백엔드 응답 머리 파서에 제어 문자·홀로 쓴 LF 검사가 없고, `Transfer-Encoding: chunked`와 `Content-Length`가 함께 오면 둘 다 돌려준다. proxy에서 둘 다 오면 chunked를 우선(또는 502)하기로 정한다.
- (http-parser) 파싱이 실패하면 `raw`만 해제되고 `method_str`, `target` 등은 해제된 메모리를 가리킨 채 남는다. 실패한 `req`의 문자열 필드는 읽지 않는다.
- (http-parser) `OPTIONS * HTTP/1.1`(asterisk-form)은 400을 받는다.
- (http-parser) 헤더가 정확히 64개, 요청 라인이 정확히 8192바이트인 경계 중 헤더 개수 경계 테스트가 없다 (요청 라인 경계는 테스트함).
- (http-parser) absolute-form 요청에서 URI 안의 호스트가 Host 헤더보다 우선하지 않는다. 경로로만 라우팅하므로 영향이 없다.
- (http-parser) 요청 라인이 8192바이트 뒤에 `\rX`처럼 CR 다음에 LF가 아닌 바이트가 오면 414가 난다. 어차피 잘못된 요청이고, 끊어 도착하든 한 번에 도착하든 결과가 같다.
- (static-server) 느린 클라이언트가 `client_timeout`보다 짧은 간격으로 1바이트씩 보내면 연결을 오래 붙잡을 수 있다. 머리 16KB 한도가 상한이다.
- (static-server) 읽는 도중 파일이 줄어 `pread`가 실패하면 접근 로그 없이 연결만 닫는다.
- (static-server) `static_resolve`의 `realpath`와 `open` 사이에 경합 틈이 있다. root 안을 바꿀 수 있는 쪽은 위협 모델 밖으로 본다.
- (static-server) `.`으로 시작하는 모든 경로 조각(`.well-known` 포함)에 403을 준다. 숨김 파일을 막으려는 의도다.
- (static-server) root가 `/`이면 root 안 판정이 의도와 다르게 동작할 수 있다.
- (reverse-proxy) 백엔드 응답이 `Transfer-Encoding: chunked`이고 클라이언트가 HTTP/1.0이면 chunked 본문을 그대로 전달한다.
- (reverse-proxy) chunked 응답의 마지막 청크(`0\r\n\r\n`)를 해석하지 않고 백엔드가 닫을 때까지 기다린다. 백엔드에 `Connection: close`를 보내므로 정상 백엔드에서는 문제없다.
- (reverse-proxy) `101 Switching Protocols`는 1xx 건너뛰기에서 제외되어 일반 응답처럼 처리된다. Upgrade 헤더를 전달하지 않으므로 쓸 일이 없다.
- (reverse-proxy) 읽기를 멈춘 백엔드 연결에도 HUP/ERR 이벤트는 오므로 256KB 한도를 조금 넘겨 읽을 수 있다 (소켓에 남은 만큼으로 끝남).
- (reverse-proxy) 클라이언트가 같은 값의 Content-Length를 중복해서 보내면 백엔드로 두 번 전달된다.
- (reverse-proxy) 데모 백엔드의 accept 루프는 EMFILE 같은 오류에서 대기 없이 돈다. 테스트 상대역이라 영향이 작다.
- (reverse-proxy) 백엔드 응답을 기다리는 동안 클라이언트가 FIN으로 정상 종료한 것은 쓰기를 시도하거나 타임아웃이 날 때까지 알아채지 못한다 (RST/HUP은 바로 닫음).
- (reverse-proxy) `ev_mod`가 실패하면 interest가 `(unsigned)-1`로 남아 읽기 재개 시 `last_active` 갱신 조건을 비껴갈 수 있다. epoll_ctl 실패는 거의 없고 그때 연결은 이미 비정상이다.
- (load) 정적 파일의 `realpath`/`open`/`pread`는 이벤트 루프 스레드에서 블로킹으로 실행된다. 저장소가 WSL의 `/mnt/c`(9p)에 있을 때 정적 요청 하나가 약 2~3ms 걸려 서버 전체 처리량이 약 300 rps로 떨어지는 것을 측정했다 (같은 파일을 ext4에 두면 수천~2만 rps). 느린 파일 시스템에서는 프록시 요청도 같이 느려진다. 완화하려면 파일 I/O를 스레드 풀로 넘기거나 `sendfile`/파일 정보 캐시를 쓴다.
- (load) loadgen은 Content-Length가 없는 응답을 읽다가 수신 타임아웃(10초)이 나도 성공으로 센다. 우리 서버는 항상 Content-Length를 보내므로 실제로 걸리지 않는다.
- (load) loadgen은 스레드 생성에 실패해도 종료 코드 0이고, 결과 줄에는 요청한 `c`가 그대로 찍힌다 (경고는 stderr에만 나감).
- (load) loadgen의 지연은 연결 시간을 포함하지 않는다 (keep-alive 대상에서는 맞음). `Connection:close`(공백 없음)는 놓치고, 호스트는 IP만 받는다.
- (load) load.sh는 서버·백엔드 프로세스가 살아 있는지(`kill -0`)까지는 확인하지 않고 포트가 열렸는지만 본다. 포트는 무작위라 충돌은 드물다.
- (load) `make valgrind`는 주 서버와 프록시 서버의 종료 코드만 검사한다 (설정 파일 서버는 보지 않음).
- (access-log) 접근 로그를 줄 단위 버퍼링으로 바꿔 로그 한 줄마다 write 시스콜이 하나 늘어난다. 처리량 영향은 측정하지 않았다. `-q`로 끌 수 있다.
- (workers) 워커 모드에서 Round-Robin 순서, 죽은 백엔드 표시, `max_conns`가 워커마다 따로다. 전체 연결 한도는 `max_conns × workers`까지 열린다.
- (workers) 워커를 다시 띄우다 fork가 실패하면 그 자리는 다시 시도하지 않는다. 마지막 워커가 그렇게 사라지면 master가 종료 코드 0으로 끝난다.
- (workers) 다시 띄우기에 지연이 없다. 워커가 시작하자마자 매번 크래시하면 master가 쉬지 않고 fork를 반복한다.
- (workers) 종료 중에는 워커가 각자 listen 소켓을 닫지만 master는 끝날 때까지 들고 있다. 그 사이에 온 연결은 backlog에서 기다리다 master가 끝날 때 RST를 받는다.
- (workers) master가 SIGKILL로 죽으면 워커가 고아로 남아 포트를 계속 잡는다 (`PR_SET_PDEATHSIG` 없음).
- (workers) 모든 워커가 listen 소켓 하나를 같이 보고 `EPOLLEXCLUSIVE`도 `SO_REUSEPORT`도 쓰지 않는다. 연결 하나에 워커가 모두 깨어나고(thundering herd), 연결이 한 워커에 몰릴 수 있다. `make load`는 워커 모드를 재지 않는다.
- (workers) 파이프로 보낸 접근 로그는 PIPE_BUF(4096바이트)보다 긴 줄이 워커끼리 섞일 수 있다 (target은 약 8KB까지 들어옴).
- (workers) 기능 테스트는 워커가 다시 떴는지만 보고, 응답이 새 워커에서 왔는지는 보지 않는다. start_server의 준비 확인은 listen 소켓만 보므로 워커가 떴다는 증거가 아니다.
