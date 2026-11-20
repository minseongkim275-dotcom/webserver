#!/usr/bin/env bash
# 🟦 공통: check.sh가 source해서 쓰는 도우미. 프로젝트마다 다시 짜지 않도록 여기 모아 둔다.
#   check_lock              여러 세션이 동시에 검사하지 않도록 잠근다 (worktree끼리도 공유)
#   in_linux <명령...>      Linux에서는 바로, Windows(Git Bash)에서는 WSL로 넘겨 실행한다
#   npm_install_if_changed  package.json·lock 파일이 바뀌었을 때만 npm ci를 돌린다

# 잠금 폴더를 모든 worktree가 공유하는 .git 아래에 둔다.
# 같은 포트를 쓰는 테스트, 같은 node_modules를 지우는 설치가 서로 부딪히지 않게 한다.
check_lock() {
  local lock locked=0
  lock="$(git rev-parse --git-common-dir 2>/dev/null || echo .)/check.lock"
  # 10분 넘은 잠금은 비정상 종료로 남은 것으로 보고 치운다.
  if [ -d "$lock" ] && [ -n "$(find "$lock" -maxdepth 0 -mmin +10 2>/dev/null)" ]; then
    rmdir "$lock" 2>/dev/null || true
  fi
  for _ in $(seq 1 180); do
    if mkdir "$lock" 2>/dev/null; then locked=1; break; fi
    sleep 1
  done
  if [ "$locked" -eq 1 ]; then
    # shellcheck disable=SC2064
    trap "rmdir '$lock' 2>/dev/null || true" EXIT
  else
    echo "경고: 3분 동안 잠금($lock)을 얻지 못해 잠금 없이 진행한다." >&2
  fi
}

# Linux 전용 도구(gcc, make, POSIX API)가 필요한 프로젝트용.
# 환경 변수는 `in_linux env CFLAGS="-Werror" make test`처럼 env로 넘긴다.
# WSL 배포판 이름은 CHECK_WSL_DISTRO로 바꿀 수 있다.
in_linux() {
  if [ "$(uname -s)" = "Linux" ]; then
    "$@"
    return
  fi
  local distro="${CHECK_WSL_DISTRO:-Ubuntu-22.04}"
  if ! command -v wsl.exe >/dev/null 2>&1; then
    echo "Linux가 아니고 WSL도 없어. Linux 또는 WSL($distro)에서 실행해." >&2
    return 1
  fi
  MSYS_NO_PATHCONV=1 wsl.exe -d "$distro" --cd "$(pwd -W)" -- "$@"
}

# 의존성이 그대로면 설치를 건너뛴다. (매번 npm ci를 돌리면 느리고, 병렬 세션이 서로의 node_modules를 지운다)
npm_install_if_changed() {
  local stamp="node_modules/.install-stamp" want
  want=$(cat package.json package-lock.json 2>/dev/null | sha1sum | cut -d' ' -f1)
  if [ -d node_modules ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
    echo "의존성 그대로: 설치 건너뜀"
    return
  fi
  if [ -f package-lock.json ]; then npm ci; else npm install; fi
  echo "$want" > "$stamp"
}
