# 고치지 않기로 한 지적

리뷰의 "선택" 항목을 한 줄씩 적는다. 리뷰어는 여기 적힌 항목을 다시 올리지 않는다.

- (http-parser) 백엔드 응답 머리 파서에 제어 문자·홀로 쓴 LF 검사가 없고, `Transfer-Encoding: chunked`와 `Content-Length`가 함께 오면 둘 다 돌려준다. proxy에서 둘 다 오면 chunked를 우선(또는 502)하기로 정한다.
- (http-parser) 파싱이 실패하면 `raw`만 해제되고 `method_str`, `target` 등은 해제된 메모리를 가리킨 채 남는다. 실패한 `req`의 문자열 필드는 읽지 않는다.
- (http-parser) `OPTIONS * HTTP/1.1`(asterisk-form)은 400을 받는다.
- (http-parser) 헤더가 정확히 64개, 요청 라인이 정확히 8192바이트인 경계 중 헤더 개수 경계 테스트가 없다 (요청 라인 경계는 테스트함).
- (http-parser) absolute-form 요청에서 URI 안의 호스트가 Host 헤더보다 우선하지 않는다. 경로로만 라우팅하므로 영향이 없다.
- (http-parser) 요청 라인이 8192바이트 뒤에 `\rX`처럼 CR 다음에 LF가 아닌 바이트가 오면 414가 난다. 어차피 잘못된 요청이고, 끊어 도착하든 한 번에 도착하든 결과가 같다.
