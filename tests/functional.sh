#!/usr/bin/env bash
# 기능 테스트: 실제 서버를 띄우고 curl과 raw 소켓(/dev/tcp)으로 확인한다.
# 사용: BUILD=build bash tests/functional.sh   (make functional이 부른다)
set -u
cd "$(dirname "$0")/.."
BUILD=${BUILD:-build}
WRAP=${WRAP:-}   # 예: WRAP="valgrind -q --leak-check=full --error-exitcode=99" (make valgrind)
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
    $WRAP "$BIN" -p "$port" "$@" >"$WORK/server-$port.log" 2>&1 &
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

# ── Reverse Proxy ──
BE=$((PORT + 10))
BE2=$((PORT + 11))
PPORT=$((PORT + 12))
purl() { echo "http://127.0.0.1:$PPORT$1"; }
LAST_PID=
start_backend() { # 인자: 포트, 이름. 시작한 pid는 LAST_PID
    "$BUILD/demo-backend" -p "$1" -n "$2" >"$WORK/be-$1.log" 2>&1 &
    LAST_PID=$!
    PIDS+=("$LAST_PID")
    for _ in $(seq 1 50); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
start_backend "$BE" a && BA_PID=$LAST_PID || bad "백엔드 a 시작 실패"
start_backend "$BE2" b && BB_PID=$LAST_PID || bad "백엔드 b 시작 실패"
printf 'listen %s\nroot %s\nupstream_timeout 2\nclient_timeout 5\nupstream api 127.0.0.1:%s 127.0.0.1:%s\nroute /api api\n' \
    "$PPORT" "$WWW" "$BE" "$BE2" >"$WORK/proxy.conf"
if start_server "$PPORT" -c "$WORK/proxy.conf" -q -p "$PPORT"; then
    ok
    PROXY_PID=${PIDS[${#PIDS[@]} - 1]}

    # 라우팅: /api는 백엔드로, 나머지는 프록시가 직접 서빙
    expect "프록시가 정적 파일을 직접 서빙" "$(curl -s "$(purl /a.txt)")" "hello"
    expect "동적 처리: sum" "$(curl -s "$(purl '/api/sum?a=2&b=40')")" '{"sum":42}'
    expect "백엔드의 404를 그대로 전달" "$(code "$(purl /api/nope)")" "404"
    expect "POST 본문 전달 (echo)" "$(curl -s -d 'payload=1' "$(purl /api/echo)")" "payload=1"
    head -c 600000 /dev/urandom >"$WORK/post.bin"
    curl -s --data-binary @"$WORK/post.bin" -o "$WORK/echo.bin" "$(purl /api/echo)"
    cmp -s "$WORK/post.bin" "$WORK/echo.bin" && ok || bad "600KB POST 본문이 그대로 돌아오지 않았다"
    expect "413 큰 본문 (프록시 경로)" "$(code -X POST -d x --header 'Content-Length: 99999999' "$(purl /api/echo)")" "413"
    expect "HEAD" "$(curl -s -I "$(purl /api/hello)" | tr -d '\r' | head -n1)" "HTTP/1.1 200 OK"
    expect "keep-alive로 프록시 연결 재사용" "$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' "$(purl /api/hello)" "$(purl /api/hello)")" "1 0 "

    # Round-Robin
    names=$(for _ in 1 2 3 4; do curl -s "$(purl /api/hello)" | grep -o '"backend":"[ab]"'; done | sort | uniq -c | tr -s ' ')
    expect "Round-Robin: a 두 번, b 두 번" "$names" ' 2 "backend":"a"
 2 "backend":"b"'
    seq1=$(for _ in 1 2 3 4; do curl -s "$(purl /api/hello)" | grep -o '"backend":"[ab]"'; done | uniq | wc -l)
    expect "Round-Robin: 번갈아 간다" "$seq1" "4"

    # 백엔드로 가는 헤더
    hdrs=$(curl -s -H 'Connection: keep-alive' -H 'X-Forwarded-For: 9.9.9.9' -H 'Expect:' "$(purl /api/headers)")
    expect_has "X-Forwarded-For를 덧붙임" "$hdrs" "X-Forwarded-For: 9.9.9.9, 127.0.0.1"
    expect_has "X-Forwarded-Proto" "$hdrs" "X-Forwarded-Proto: http"
    expect_has "백엔드에는 Connection: close" "$hdrs" "Connection: close"

    # 응답 형식
    expect "chunked 응답 본문" "$(curl -s "$(purl /api/chunked)")" "hello world"
    expect_has "chunked 응답은 그대로 전달" "$(curl -s -D - -o /dev/null "$(purl /api/chunked)")" "Transfer-Encoding: chunked"
    expect_has "길이를 모르는 응답 뒤에는 닫는다" "$(curl -s -D - -o /dev/null "$(purl /api/chunked)")" "Connection: close"
    expect "3MB 본문 중계" "$(curl -s "$(purl '/api/big?kb=3000')" | wc -c | tr -d ' ')" "3072000"

    # 타임아웃과 실패
    t0=$SECONDS
    expect "504 백엔드 응답 없음 (upstream_timeout 2초)" "$(code "$(purl '/api/slow?ms=4500')")" "504"
    [ $((SECONDS - t0)) -le 4 ] && ok || bad "504가 너무 늦다: $((SECONDS - t0))초"
    expect "느리지만 시간 안에 오면 200" "$(code "$(purl '/api/slow?ms=300')")" "200"
    expect "502 백엔드가 응답 없이 끊음" "$(code "$(purl /api/drop)")" "502"

    # 클라이언트가 중계 도중 끊는다
    curl -s -m 1 --limit-rate 100k -o /dev/null "$(purl '/api/big?kb=20000')"
    sleep 0.3
    expect "중계 중 클라이언트가 끊어도 서버가 산다" "$(code "$(purl /api/hello)")" "200"

    # 느린 클라이언트: upstream_timeout(2초)보다 오래 읽지 않다가 읽기 시작해도 응답이 잘리지 않아야 한다.
    # (읽기를 재개하는 순간 백엔드 타임아웃 시계가 이미 지나 있으면 끊기던 문제. 확률적이라 세 명을 동시에 시험한다.)
    sp=()
    for d in 3.2 3.5 3.8; do
        (
            exec 5<>"/dev/tcp/127.0.0.1/$PPORT"
            printf 'GET /api/big?kb=4096 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' >&5
            sleep "$d"
            timeout 10 cat <&5 | wc -c >"$WORK/slow-$d.n"
        ) &
        sp+=($!)
    done
    wait "${sp[@]}"
    for d in 3.2 3.5 3.8; do
        [ "$(cat "$WORK/slow-$d.n" 2>/dev/null || echo 0)" -ge 4194304 ] && ok || bad "$d초 쉬고 읽은 클라이언트가 4MB를 다 받지 못했다: $(cat "$WORK/slow-$d.n") 바이트"
    done

    # 동시 요청
    conc=$(seq 1 300 | xargs -P 40 -I{} curl -s -o /dev/null -w '%{http_code}\n' "$(purl /api/hello)" | sort | uniq -c | tr -s ' ')
    expect "프록시 동시 300개 요청이 모두 200" "$conc" " 300 200"

    # 백엔드 장애: a를 죽여도 b로 넘어간다
    kill -KILL "$BA_PID" 2>/dev/null
    wait "$BA_PID" 2>/dev/null
    only=$(seq 1 6 | xargs -I{} curl -s -o /dev/null -w '%{http_code}\n' "$(purl /api/hello)" | sort | uniq -c | tr -s ' ')
    expect "백엔드 a가 죽어도 모든 요청이 200 (b로 재시도)" "$only" " 6 200"
    expect_has "죽은 백엔드로는 가지 않는다" "$(curl -s "$(purl /api/hello)")" '"backend":"b"'
    # 둘 다 죽으면 502
    kill -KILL "$BB_PID" 2>/dev/null
    wait "$BB_PID" 2>/dev/null
    expect "502 백엔드가 모두 죽음" "$(code "$(purl /api/hello)")" "502"
    expect "백엔드가 없어도 정적 파일은 서빙" "$(curl -s "$(purl /a.txt)")" "hello"
    # 되살아난 백엔드는 (down 표시가 남아 있어도) 다시 쓰인다
    start_backend "$BE" a && ok || bad "백엔드 a 재시작 실패"
    expect "백엔드가 돌아오면 다시 200" "$(code "$(purl /api/hello)")" "200"

    # 프록시도 SIGTERM에 정상 종료한다 (ASan이면 누수 없이)
    kill -TERM "$PROXY_PID"
    wait "$PROXY_PID" 2>/dev/null
    expect "프록시 SIGTERM 종료 코드" "$?" "0"
else
    bad "프록시 서버를 시작하지 못했다: $(head -n 3 "$WORK/server-$PPORT.log" 2>/dev/null)"
fi

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

# ── 접근 로그: 파일로 보내도 서버가 도는 중에 한 줄씩 보여야 한다 (-q 없이) ──
PORT3=$((PORT + 3))
if start_server "$PORT3" -r "$WWW"; then
    curl -s -o /dev/null "http://127.0.0.1:$PORT3/a.txt"
    curl -s -o /dev/null "http://127.0.0.1:$PORT3/nope"
    sleep 0.3
    alog=$(cat "$WORK/server-$PORT3.log")
    expect_has "접근 로그(200)가 서버가 도는 중에 보인다" "$alog" '"GET /a.txt HTTP/1.1" 200 '
    expect_has "접근 로그(404)" "$alog" '"GET /nope HTTP/1.1" 404 '
else
    bad "로그 확인용 서버를 시작하지 못했다"
fi

# ── 워커 모드: master 하나 + 워커 N개. 죽은 워커는 다시 띄우고, SIGTERM은 워커까지 정리한다 ──
PORT4=$((PORT + 4))
printf 'workers 2\n' >"$WORK/workers.conf"
if start_server "$PORT4" -c "$WORK/workers.conf" -r "$WWW" -q; then
    MPID=${PIDS[${#PIDS[@]} - 1]}
    sleep 0.3
    expect "워커 2개" "$(pgrep -P "$MPID" | wc -l)" "2"
    expect "워커가 응답" "$(curl -s "http://127.0.0.1:$PORT4/a.txt")" "hello"
    victim=$(pgrep -P "$MPID" | head -n1)
    kill -KILL "$victim"
    # 죽은 워커는 master가 거둘 때까지 좀비로 pgrep에 남으므로, 그 pid가 사라질 때까지 기다린다
    for _ in $(seq 1 30); do
        if ! pgrep -P "$MPID" | grep -qx "$victim" && [ "$(pgrep -P "$MPID" | wc -l)" = 2 ]; then break; fi
        sleep 0.1
    done
    expect "죽은 워커를 다시 띄움" "$(pgrep -P "$MPID" | wc -l)" "2"
    expect "다시 띄운 뒤에도 응답" "$(curl -s "http://127.0.0.1:$PORT4/a.txt")" "hello"
    WPIDS=$(pgrep -P "$MPID")
    kill -TERM "$MPID"
    wait "$MPID" 2>/dev/null
    expect "워커 모드 SIGTERM 종료 코드" "$?" "0"
    left=0
    for w in $WPIDS; do kill -0 "$w" 2>/dev/null && left=$((left + 1)); done
    expect "종료 뒤 남은 워커" "$left" "0"
    PIDS[${#PIDS[@]} - 1]=""
else
    bad "워커 모드 서버를 시작하지 못했다: $(head -n 3 "$WORK/server-$PORT4.log" 2>/dev/null)"
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
