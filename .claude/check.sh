#!/usr/bin/env bash
# 🟩 특화: 이 프로젝트의 "통과/실패" 기준. 공통 게이트(Stop 훅, CI)는 이 파일의 종료 코드만 본다.
# 경고는 -Werror로 오류로 다룬다 (팀 규칙 "경고가 늘어난 PR은 병합하지 않는다").
set -euo pipefail
cd "$(dirname "$0")/.."
source .claude/hooks/check-lib.sh
check_lock

# ── 프로젝트 검사 (🟩) ── C + make. Linux 전용 코드(epoll)라 Windows에서는 WSL로 넘긴다.
in_linux env CFLAGS="-O2 -g -Werror" make test
