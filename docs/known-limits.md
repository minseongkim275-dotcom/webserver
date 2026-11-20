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
