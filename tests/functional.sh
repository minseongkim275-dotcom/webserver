#!/usr/bin/env bash
# 기능 테스트: 실제 서버를 띄우고 curl과 raw 소켓(/dev/tcp)으로 확인한다.
# 사용: BUILD=build bash tests/functional.sh   (make functional이 부른다)
set -u
cd "$(dirname "$0")/.."
BUILD=${BUILD:-build}
BIN="$BUILD/webserver"
PORT=${PORT:-$((10000 + RANDOM % 20000))}   # 임시 포트 범위(32768~)와 겹치지 않게
PORT2=$((PORT + 1))
WORK=$(mktemp -d)
WWW="$WORK/www"
PIDS=()
pass=0
fail=0

cleanup() {
    for p in "${PIDS[@]:-}"; do
        [ -n "$p" ] && kill -TERM "$p" 2>/dev/null && { wait "$p"; } 2>/dev/null
    done
    chmod -R u+rwx "$WORK" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "  FAIL: $1" >&2; }
expect() { if [ "$2" = "$3" ]; then ok; else bad "$1: got '$2', want '$3'"; fi; }
expect_has() { case "$2" in *"$3"*) ok ;; *) bad "$1: '$3' not found in: $(printf '%s' "$2" | head -c 300)" ;; esac; }

url() { echo "http://127.0.0.1:$PORT$1"; }
code() { curl -s -o /dev/null -w '%{http_code}' --path-as-is "$@"; }
header() { curl -s -D - -o /dev/null "$2" | tr -d '\r' | grep -i "^$1:" | cut -d' ' -f2-; }
# raw 요청을 보내고 응답 전체를 돌려준다. 요청에 %가 들어가면 안 된다. 서버가 닫지 않으면 3초 뒤 끊는다.
raw() {
    local port=${2:-$PORT}
    exec 3<>"/dev/tcp/127.0.0.1/$port" || return 1
    printf "$1" >&3
    timeout 3 cat <&3
    exec 3<&- 3>&-
}
first_line() { printf '%s' "$1" | head -n1 | tr -d '\r'; }

start_server() { # 인자: 포트, 나머지는 서버 옵션. 시작하면 PIDS의 마지막에 pid가 들어간다
    local port=$1
    shift
    "$BIN" -p "$port" "$@" >"$WORK/server-$port.log" 2>&1 &
    local pid=$!
    PIDS+=("$pid")
    for _ in $(seq 1 50); do
        kill -0 "$pid" 2>/dev/null || return 1
        (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# ── 준비: 임시 root ──
mkdir -p "$WWW/sub" "$WWW/empty" "$WORK/outside"
echo "<h1>home</h1>" >"$WWW/index.html"
echo "body{}" >"$WWW/style.css"
echo "var a=1;" >"$WWW/app.js"
echo "hello" >"$WWW/a.txt"
echo "sub-index" >"$WWW/sub/index.html"
echo "hidden" >"$WWW/.secret"
echo "outside-secret" >"$WORK/outside/passwd"
base64 -d >"$WWW/logo.png" <<<"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=="
head -c 5000000 /dev/urandom >"$WWW/big.bin"
head -c 3000000 /dev/urandom >"$WORK/upload.bin"
ln -s "$WORK/outside" "$WWW/link_out"
echo "noperm" >"$WWW/noperm.txt"
chmod 000 "$WWW/noperm.txt"

if ! start_server "$PORT" -r "$WWW" -q; then
    echo "서버가 시작되지 않았다:" >&2
    cat "$WORK/server-$PORT.log" >&2
    exit 1
fi
MAIN_PID=${PIDS[0]}

# ── 정적 파일 ──
expect "GET /" "$(curl -s "$(url /)")" "<h1>home</h1>"
expect "index.html Content-Type" "$(header Content-Type "$(url /)")" "text/html; charset=utf-8"
expect "css Content-Type" "$(header Content-Type "$(url /style.css)")" "text/css; charset=utf-8"
expect "js Content-Type" "$(header Content-Type "$(url /app.js)")" "text/javascript; charset=utf-8"
expect "png Content-Type" "$(header Content-Type "$(url /logo.png)")" "image/png"
expect "Content-Length" "$(header Content-Length "$(url /a.txt)")" "6"
expect "Server 헤더" "$(header Server "$(url /a.txt)")" "jungle-webserver"
expect_has "Date 헤더" "$(header Date "$(url /a.txt)")" "GMT"
expect "sub/ 의 index" "$(curl -s "$(url /sub/)")" "sub-index"
expect "쿼리 문자열은 무시" "$(curl -s "$(url '/a.txt?x=1&y=2')")" "hello"
expect "HEAD 는 본문이 없다" "$(curl -s -I "$(url /a.txt)" | tr -d '\r' | head -n1)" "HTTP/1.1 200 OK"
expect "HEAD Content-Length" "$(curl -s -I "$(url /big.bin)" | tr -d '\r' | grep -i '^content-length' | cut -d' ' -f2)" "5000000"
curl -s -o "$WORK/got.png" "$(url /logo.png)"
cmp -s "$WORK/got.png" "$WWW/logo.png" && ok || bad "png 내용이 다르다"
curl -s -o "$WORK/got.bin" "$(url /big.bin)"
cmp -s "$WORK/got.bin" "$WWW/big.bin" && ok || bad "5MB 파일 내용이 다르다 (부분 전송 처리)"

# ── 오류 응답 ──
expect "404" "$(code "$(url /nope.html)")" "404"
expect "404 본문" "$(curl -s "$(url /nope.html)")" "$(curl -s "$(url /nope2.html)")"
expect "빈 디렉터리는 404" "$(code "$(url /empty/)")" "404"
expect "403 .." "$(code "$(url /../outside/passwd)")" "403"
expect "403 %2e%2e" "$(code "$(url /%2e%2e/outside/passwd)")" "403"
expect "403 중간의 .." "$(code "$(url /sub/../a.txt)")" "403"
expect "403 숨김 파일" "$(code "$(url /.secret)")" "403"
expect "403 root 밖 심볼릭 링크" "$(code "$(url /link_out/passwd)")" "403"
[ "$(id -u)" != 0 ] && expect "403 권한 없는 파일" "$(code "$(url /noperm.txt)")" "403"
expect "301 슬래시 없는 디렉터리" "$(code "$(url /sub)")" "301"
expect "301 Location" "$(header Location "$(url '/sub?x=1')")" "/sub/?x=1"
expect "301 앞의 슬래시를 줄임" "$(curl -s -D - -o /dev/null --path-as-is "$(url //sub)" | tr -d '\r' | grep -i '^location' | cut -d' ' -f2)" "/sub/"
expect "405 POST" "$(curl -s -o /dev/null -w '%{http_code}' -X POST -d x=1 "$(url /a.txt)")" "405"
expect_has "405 Allow" "$(curl -s -D - -o /dev/null -X POST "$(url /a.txt)")" "Allow: GET, HEAD"
# 405 뒤에도 연결을 닫는다 (design.md: 4xx/5xx 오류 응답 뒤에는 닫는다). 뒤따르는 GET은 처리되지 않아야 한다.
r405=$(raw 'POST /a.txt HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
expect_has "405 뒤 Connection: close" "$r405" "Connection: close"
expect "405 뒤 같은 연결의 요청은 처리하지 않는다" "$(printf '%s' "$r405" | grep -c '^HTTP/1.1 200')" "0"

# ── 잘못된 요청 (raw) ──
expect "400 깨진 요청 라인" "$(first_line "$(raw 'garbage\r\n\r\n')")" "HTTP/1.1 400 Bad Request"
expect "400 Host 없음" "$(first_line "$(raw 'GET / HTTP/1.1\r\n\r\n')")" "HTTP/1.1 400 Bad Request"
expect "501 모르는 메서드" "$(first_line "$(raw 'BREW / HTTP/1.1\r\nHost: x\r\n\r\n')")" "HTTP/1.1 501 Not Implemented"
expect "505 버전" "$(first_line "$(raw 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')")" "HTTP/1.1 505 HTTP Version Not Supported"
LONG=$(head -c 9000 /dev/zero | tr '\0' a)
expect "414 긴 URI" "$(first_line "$(raw "GET /$LONG HTTP/1.1\r\nHost: x\r\n\r\n")")" "HTTP/1.1 414 URI Too Long"
expect "431 긴 헤더" "$(first_line "$(raw "GET / HTTP/1.1\r\nHost: x\r\nX-A: $(head -c 20000 /dev/zero | tr '\0' b)\r\n\r\n")")" "HTTP/1.1 431 Request Header Fields Too Large"
expect "413 큰 본문 선언" "$(first_line "$(raw 'POST /a.txt HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999\r\n\r\n')")" "HTTP/1.1 413 Content Too Large"
expect "413 실제 3MB 업로드를 curl이 응답으로 받는다" "$(curl -s -o /dev/null -w '%{http_code}' --data-binary @"$WORK/upload.bin" "$(url /a.txt)")" "413"
expect_has "오류 뒤에는 닫는다" "$(raw 'BREW / HTTP/1.1\r\nHost: x\r\n\r\n')" "Connection: close"

# ── 연결 유지와 파이프라이닝 ──
expect "keep-alive로 연결을 재사용" "$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$(url /a.txt)" "$(url /a.txt)")" "1 0 "
two=$(raw 'GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\nGET /sub/ HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
expect "파이프라이닝: 응답 두 개" "$(printf '%s' "$two" | grep -c '^HTTP/1.1 200 OK')" "2"
expect_has "파이프라이닝: 순서 유지" "$two" "hello"
expect_has "파이프라이닝: 두 번째 본문" "$two" "sub-index"
expect_has "Connection: close 요청은 닫는다" "$(raw 'GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')" "Connection: close"
expect_has "HTTP/1.0 은 기본이 close" "$(raw 'GET /a.txt HTTP/1.0\r\n\r\n')" "Connection: close"
# 머리만 먼저 보내고 잠시 뒤 본문을 보낸다: 서버가 그 사이에 100 Continue를 보내야 한다
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf 'POST /a.txt HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n' >&3
sleep 0.3
printf 'abcd' >&3
cont=$(timeout 3 cat <&3)
exec 3<&- 3>&-
expect_has "100-continue" "$cont" "HTTP/1.1 100 Continue"
expect_has "100-continue 뒤 최종 응답" "$cont" "HTTP/1.1 405"

# ── 클라이언트 비정상 종료 ──
(exec 3<>"/dev/tcp/127.0.0.1/$PORT"; printf 'GET / HT' >&3) 2>/dev/null
(exec 3<>"/dev/tcp/127.0.0.1/$PORT"; printf 'GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n' >&3; sleep 0.2) 2>/dev/null
curl -s -m 1 --limit-rate 100k -o /dev/null "$(url /big.bin)"
sleep 0.3
expect "끊긴 클라이언트들 뒤에도 서버가 산다" "$(code "$(url /a.txt)")" "200"
kill -0 "$MAIN_PID" 2>/dev/null && ok || bad "서버가 죽었다"

# ── 동시 요청 ──
conc=$(seq 1 300 | xargs -P 60 -I{} curl -s -o /dev/null -w '%{http_code}\n' "$(url /index.html)" | sort | uniq -c | tr -s ' ')
expect "동시 300개 요청이 모두 200" "$conc" " 300 200"
big_ok=$(seq 1 8 | xargs -P 8 -I{} sh -c "curl -s '$(url /big.bin)' | cmp -s - '$WWW/big.bin' && echo ok" | sort | uniq -c | tr -s ' ')
expect "동시 5MB 다운로드 8개가 온전함" "$big_ok" " 8 ok"

# ── 시작 옵션과 설정 파일 ──
"$BIN" -h >"$WORK/help.txt" 2>&1
expect "-h 종료 코드" "$?" "0"
expect_has "-h 사용법" "$(cat "$WORK/help.txt")" "usage: webserver"
"$BIN" -p 99999 >/dev/null 2>&1
expect "잘못된 포트는 종료 코드 2" "$?" "2"
"$BIN" -p "$PORT" -r "$WWW" -q >"$WORK/dup.log" 2>&1
expect "이미 쓰는 포트는 종료 코드 1" "$?" "1"
expect_has "bind 실패 메시지" "$(cat "$WORK/dup.log")" "bind"
"$BIN" -p "$PORT2" -r "$WORK/nonexistent" -q >/dev/null 2>&1
expect "없는 root는 종료 코드 1" "$?" "1"

printf 'listen %s\nroot %s\nclient_timeout 1\n' "$PORT2" "$WWW" >"$WORK/test.conf"
if start_server "$PORT2" -c "$WORK/test.conf" -q -p "$PORT2"; then
    ok
    exec 3<>"/dev/tcp/127.0.0.1/$PORT2"
    timeout 6 cat <&3 >/dev/null
    rc=$?
    exec 3<&- 3>&-
    expect "조용한 연결은 client_timeout 뒤에 서버가 닫는다 (timeout이면 124)" "$rc" "0"
    expect "설정 파일의 root로 서빙" "$(curl -s "http://127.0.0.1:$PORT2/a.txt")" "hello"
else
    bad "설정 파일로 서버를 시작하지 못했다: $(cat "$WORK/server-$PORT2.log" 2>/dev/null | head -n 3)"
fi

# ── 종료 처리: 쉬는 연결이 있어도 SIGTERM에 바로 정상 종료 ──
exec 4<>"/dev/tcp/127.0.0.1/$PORT"
printf 'GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n' >&4
sleep 0.2
start=$SECONDS
kill -TERM "$MAIN_PID"
wait "$MAIN_PID" 2>/dev/null
expect "SIGTERM 종료 코드" "$?" "0"
[ $((SECONDS - start)) -le 3 ] && ok || bad "종료에 $((SECONDS - start))초 걸렸다"
exec 4<&- 4>&-
PIDS[0]=""

echo "functional.sh: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
