# 공통 하네스

새 프로젝트에 복사해 쓰는 Claude Code 하네스. mini-react 회고(2026-11-12)에서 나온 문제를 반영했고, mini-dbms와 mini-redis에 먼저 적용했다.

## 쓰는 법

1. 이 폴더의 내용을 새 저장소 루트에 복사한다 (`README.md`는 빼고, 숨김 폴더 `.claude`, `.github` 포함).
2. `.claude/check.sh` 맨 아래 "프로젝트 검사"를 채운다. 경고를 오류로 다루는 옵션을 넣는다.
3. `CLAUDE.md`의 `<…>` 자리를 채운다. 도메인 규칙마다 검사하는 테스트를 적는다.
4. `.claude/review-checklist.md`에 프로젝트 리뷰 항목 3~7줄을 적는다.
5. `.claude/settings.json`
   - `allow`에 프로젝트 명령을 추가한다 (예: `Bash(make:*)`, `Bash(npm test:*)`, `Bash(npm run:*)`).
   - 개인 fork 원격이 `origin`이 아니면 `deny`에 `Bash(git push <원격 이름> main:*)`을 추가한다.
6. `.gitignore` 맨 아래와 `ci.yml`의 표시된 자리에 프로젝트 추가분을 넣는다.
7. 과제 요구사항 원문을 `docs/requirements.md`로 저장한다.
8. GitHub에서 아래 main 보호 체크리스트를 설정한다.

## 파일 구분

| 표시 | 뜻 |
|---|---|
| 🟦 공통 | 모든 프로젝트에 같은 내용. 고치면 이 폴더에도 반영한다 |
| 🟩 특화 | 프로젝트마다 채우는 값 |

| 파일 | 구분 | 역할 |
|---|---|---|
| `CLAUDE.md` | 🟩 | 프로젝트 설명, 빌드 명령, 폴더, 도메인 규칙 표 |
| `.claude/team-rules.md` | 🟦 | AI 사용 범위, 브랜치·커밋·PR, 리뷰, 문서, 결과 확인, 하네스 갱신 |
| `.claude/settings.json` | 🟦 + 🟩 | 권한(allow·ask·deny)과 훅 등록 |
| `.claude/check.sh` | 🟩 | 통과/실패 기준. Stop 훅과 CI가 이 파일의 종료 코드만 본다 |
| `.claude/hooks/check-lib.sh` | 🟦 | `check.sh`용 도우미: 잠금, WSL 실행, 설치 건너뛰기 |
| `.claude/hooks/quality-gate.sh` | 🟦 | Stop 훅. 끝내기 전에 `check.sh`를 돌린다 |
| `.claude/hooks/block-main-push.sh` | 🟦 | main으로 가는 push 차단 |
| `.claude/hooks/json-field.sh` | 🟦 | 훅 입력 JSON에서 값 꺼내기 |
| `.claude/agents/reviewer.md` | 🟦 | 리뷰어. 심각도·재현 여부·종료 권고 |
| `.claude/agents/impact-investigator.md` | 🟦 | 영향 범위 조사 |
| `.claude/review-checklist.md` | 🟩 | 리뷰어가 추가로 볼 프로젝트 항목 |
| `.claude/skills/work-on-issue/` | 🟦 | 코드 변경 절차 |
| `.claude/skills/parallel-sessions/` | 🟦 | 여러 세션을 띄울 때의 역할·격리·정리 |
| `.github/workflows/ci.yml` | 🟦 + 🟩 | main·dev의 PR과 push에서 `check.sh` 실행 |
| `.gitignore`, `.gitattributes` | 🟦 + 🟩 | 비밀 파일·개인 설정 제외, 줄 끝 LF 고정 |

## 이렇게 만든 이유

| 정한 것 | 막으려는 문제 (mini-react에서 실제로 일어난 일) |
|---|---|
| 지켜야 하는 것은 훅·deny·테스트·CI로 강제한다 | 글로만 적은 절차(스킬 순서, PR 리뷰)는 바쁜 날 사라졌다. 끝까지 지켜진 것은 기계가 강제한 규칙뿐 |
| 게이트는 세션이 실제로 일하는 폴더에서 검사한다 | worktree 세션의 게이트가 본 체크아웃을 검사했다 |
| `check.sh`는 잠금을 잡고, 의존성이 그대로면 설치를 건너뛴다 | 게이트 실패 12건 중 9건이 병렬 세션의 설치·포트 충돌이었다 |
| 실패 테스트 단계 표시(`.claude/logs/red-phase`, 30분) | 실패하는 테스트를 먼저 쓰면 게이트가 막았다 |
| 기준점은 `dev` 우선 | 작업은 dev에 모았는데 리뷰어와 게이트는 main을 기준으로 봤다 |
| 리뷰어 판정 기준과 종료 권고, 리뷰는 두 번까지 | 리뷰할 때마다 지적이 나와 "전부 고치기"를 반복하다 결국 리뷰를 껐다 |
| `git add/commit/switch/merge/push` 허용 | 병렬 세션이 전부 입력 대기로 멈췄다 |
| 대상을 적은 push는 main에 있어도 통과 | 원래 훅은 main에 체크아웃하면 `git push origin dev`도 막았다 |
| CI를 dev push에서도 실행 | dev는 PR 없이 병합해서 CI가 한 번도 돌지 않았다 |
| 문서에 숫자를 손으로 적지 않는다, 문서는 기능 브랜치에서 함께 | 테스트 수를 맞추는 문서 커밋이 전체의 절반 가까이였다 |
| 병렬 세션 스킬(역할·승인 범위·정리) | 지시문을 매번 새로 썼고 worktree 8개와 브랜치 60개가 남았다 |
| 도메인 규칙 표에 검사 테스트를 적는다 | 규칙이 지켜지는지 확인할 방법이 없었다 |

## main 보호 체크리스트 (GitHub Settings → Rules → Rulesets)

훅은 로컬 실수 방지용이다. 우회(훅을 끈 세션, Claude 밖의 push)는 이 규칙이 막는다.

- [ ] 대상: `main`
- [ ] Restrict deletions
- [ ] Require a pull request before merging
- [ ] Require status checks to pass → `check`
- [ ] Block force pushes

fork 저장소는 Actions 탭에서 워크플로 실행을 먼저 허용해야 CI가 돈다.

## 알아 둘 것

- **실패 테스트 단계**: `touch .claude/logs/red-phase` → 구현 → `rm .claude/logs/red-phase`. CI는 이 표시를 보지 않는다.
- **훅의 한계**: 명령 문자열에 `git push`와 `main`이 글자로만 들어 있어도 막힐 수 있다. 그럴 때는 명령을 스크립트 파일로 옮긴다.
- **Windows**: `check-lib.sh`의 `in_linux`는 WSL 배포판 `Ubuntu-22.04`를 쓴다. 다르면 환경 변수 `CHECK_WSL_DISTRO`로 바꾼다.
- **일부러 넣지 않은 것**: `.mcp.json`(GitHub MCP의 쓰기 도구는 Bash 훅을 거치지 않아 main 차단을 우회할 수 있음), `claude.yml`·`claude-review.yml`(비밀 값이 있는 러너에서 fork PR 코드를 실행할 위험), 예약 작업(프로젝트마다 대상이 다름).
- **하네스를 고쳤을 때**: 🟦 파일을 프로젝트에서 고쳤으면 이 폴더에도 같은 내용을 넣는다. 같은 지시를 두 번 하게 되면 규칙으로 옮길 때다.
