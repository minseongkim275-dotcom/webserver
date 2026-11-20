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
