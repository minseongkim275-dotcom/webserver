#!/usr/bin/env bash
# 부하 테스트: 정적 파일과 Reverse Proxy를 동시 연결 수별로 측정한다. (게이트 밖: make load)
# 같은 컴퓨터에서 서버·백엔드·부하 생성기가 CPU를 나눠 쓰므로 절대값이 아니라 추세로 읽는다.
#   DURATION=3 CONNS="1 10 100 1000" ROOT=/tmp/wsroot bash tests/load.sh
# ROOT: 정적 파일 폴더. 안 주면 www를 임시 폴더로 복사해 쓴다.
# 결과는 문서에 손으로 옮기지 않는다. 필요하면 이 명령의 출력을 그대로 붙인다.
set -u
cd "$(dirname "$0")/.."
BUILD=${BUILD:-build}
DURATION=${DURATION:-3}
CONNS=${CONNS:-"1 10 100 1000"}
PORT=${PORT:-$((10000 + RANDOM % 20000))}
BE1=$((PORT + 1))
BE2=$((PORT + 2))
WORK=$(mktemp -d)
PIDS=()
trap 'for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -TERM "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$WORK"' EXIT

ulimit -n 65536 2>/dev/null || echo "참고: fd 한도를 올리지 못했다 (ulimit -n $(ulimit -n)). 연결 1000개에서 오류가 날 수 있다." >&2

wait_port() {
    for _ in $(seq 1 50); do
        (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
        sleep 0.1
    done
    echo "포트 $1이 열리지 않았다" >&2
    exit 1
}

"$BUILD/demo-backend" -p "$BE1" -n a 2>/dev/null &
PIDS+=($!)
"$BUILD/demo-backend" -p "$BE2" -n b 2>/dev/null &
PIDS+=($!)
# 정적 파일은 로컬 임시 폴더로 복사해서 쓴다. (저장소가 WSL의 /mnt/c 같은 9p 마운트에 있으면 파일 호출이
# 요청당 약 2~3ms라 서버가 아니라 파일 시스템을 재게 된다. 이벤트 루프 안의 파일 I/O는 블로킹이다.)
if [ -z "${ROOT:-}" ]; then
    cp -r www "$WORK/www"
    ROOT="$WORK/www"
fi
printf 'listen %s\nroot %s\nmax_conns 20000\nclient_timeout 30\nupstream api 127.0.0.1:%s 127.0.0.1:%s\nroute /api api\n' \
    "$PORT" "$ROOT" "$BE1" "$BE2" >"$WORK/load.conf"
"$BUILD/webserver" -c "$WORK/load.conf" -p "$PORT" -q 2>/dev/null &
PIDS+=($!)
wait_port "$BE1"
wait_port "$BE2"
wait_port "$PORT"

echo "코어 $(nproc)개, 각 측정 ${DURATION}초, 서버 하나(단일 스레드 epoll) + 데모 백엔드 둘"
echo "-- 정적 파일 (www/index.html)"
fail=0
for c in $CONNS; do
    "$BUILD/loadgen" -u "http://127.0.0.1:$PORT/index.html" -c "$c" -d "$DURATION" || fail=1
done
echo "-- Reverse Proxy (/api/hello → 백엔드 a, b를 Round-Robin)"
for c in $CONNS; do
    "$BUILD/loadgen" -u "http://127.0.0.1:$PORT/api/hello" -c "$c" -d "$DURATION" || fail=1
done
exit $fail
