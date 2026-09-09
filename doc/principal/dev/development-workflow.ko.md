# 작업 진행 방식 — Issue · 브랜치/worktree · PR

> 적용 시작: 2026-09-10 (framework 0.11.0 릴리스 뒤, 1.0 준비부터). 사용자 결정.
> 이 문서는 "어떤 작업을 어디에 등록하고, 어느 브랜치에서 하고, 어떻게 main에 넣는가"를 소유한다.
> 커밋 메시지·버전·릴리스 절차는 [`CONTRIBUTING.ko.md`](../../../CONTRIBUTING.ko.md) §9, 에이전트
> 운영 규칙은 [`AGENTS.md`](../../../AGENTS.md)가 그대로 소유한다.

## 1. 한눈에

```
Issue 등록 ──> 브랜치 + worktree ──> 작업(사람 또는 codex job) ──> PR ──> CI + 감독자 검증 ──> merge ──> 정리
   │                                                                  │
   └── Milestone(릴리스)·Project(보드)로 묶어서 본다                  └── "Closes #N"으로 Issue 자동 종료
```

`main`은 **PR로만** 바뀐다(§6의 예외 제외). 브랜치 하나 = Issue 하나 = worktree 하나 = (있다면) codex job 하나.

## 2. 작업 등록 — GitHub Issue

- 모든 작업은 시작 전에 Issue로 등록한다. 제목은 한 줄 결과("무엇이 어떻게 되어야 한다"), 본문은
  **범위 / 완료 조건 / 근거**(FB-nnn·D-nnn 결정 기록, 측정값, 보고서 경로) 세 항목.
- 라벨은 두 축만 쓴다.
  - `area:` `core` · `bindings` · `framework-dotnet` · `framework-java` · `framework-node` · `framework-cpp` · `bench` · `ci` · `docs`
  - `kind:` `bug` · `perf` · `feature` · `chore`
- 한 Issue는 원인 하나·결과 하나다. 여러 언어에 같은 수정이 필요하면 언어별 Issue로 나누고 본문에서
  서로 링크한다(`AGENTS.md`의 "job 하나 = 원인 하나" 규칙과 같다).
- 기존 기록 체계는 그대로 둔다. 결정 기록(`doc/plan/**/decisions*.md`)의 FB/D 번호가 근거이고,
  Issue는 그것을 "해야 할 일"로 꺼내 놓은 것이다. Issue 본문에 근거 링크를 적고, 결정 기록에는
  Issue 번호를 적는다.

## 3. 묶음 — Milestone과 Project

- **Milestone** = 릴리스. `1.0`(Core 1.0 · bindings 1.0.0 · framework 1.0.0 묶음 배포)처럼 릴리스
  이름으로 만들고, 그 릴리스에 들어가야 하는 Issue·PR에 붙인다. GitHub가 "남은 일 / 닫힌 일"을 세 준다.
  릴리스 태그 전 체크리스트는 "milestone에 열린 Issue가 0"이다.
- **Project** = 보드. 조직(`zlink-systems`) 아래 하나(`ZLink`). 열은
  `Todo → In progress → Review → Done`. 사용자 정의 필드 `area`(라벨과 같은 값)와 `runner`
  (`astra` / `sol` / `direct`)로 지금 어느 job이 무엇을 하는지 본다. Issue를 열면 Todo, PR이 열리면
  Review, merge되면 Done으로 자동 이동하도록 workflow를 켠다.
- 라벨은 §2의 두 축으로 끝내고, 상태·담당은 Project 필드로만 관리한다(중복 관리 금지).

## 4. 브랜치와 worktree

- 브랜치 이름: `<area>/<issue번호>-<slug>` — 예 `framework-java/57-mesh-pump-blocking-wait`,
  `bench/61-dealer-router-rows`.
- worktree: `git worktree add ~/project/zlink-<slug> -b <branch> origin/main`. 작업 디렉터리 하나에
  브랜치 하나. 같은 브랜치를 두 worktree에서 열지 않는다.
- 로컬 패키지는 **내용 해시로 키를 잡은 공유 캐시**에서 가져온다(§4.1). worktree마다 다시 빌드하지 않는다.
- codex job에는 worktree 경로를 `-C`로 주고, **그 브랜치에만** 커밋하게 한다. push는 감독자가 한다.
  job이 `doc/**`·스펙·`.github/**`를 고치지 않는 규칙은 그대로다(BLOCKERS로 보고).
- main을 따라잡을 때는 `git fetch && git merge origin/main`(rebase는 공유 브랜치에서 쓰지 않는다).

### 4.1 로컬 패키지 공유 캐시 (content-addressed)

binding 로컬 패키지(nuget·npm·maven·C++ install)의 입력은 `bindings/` 소스 트리, `BINDINGS_VERSION`,
Core 버전 세 가지뿐이다. 같은 입력이면 어느 worktree에서 빌드했든 결과가 같으므로 해시로 공유한다
(vcpkg binary cache·Conan cache·Gradle build cache와 같은 원리).

- 키: `sha256(git rev-parse HEAD:bindings ‖ BINDINGS_VERSION ‖ core_version)` 앞 16자리.
- 위치: `~/.cache/zlink/packages/<키>/{nuget,npm,maven,install}` + 완료 마커 `.complete`(빌드 중인
  키를 다른 worktree가 읽지 않게 한다).
- worktree의 `.artifacts/wsl`은 그 디렉터리로의 symlink다. `scripts/local-package/build-wsl.sh`가
  "키가 있고 `.complete`면 링크만, 없으면 빌드 뒤 링크"를 한다. Core release prefix
  `~/.cache/zlink/core/<ver>`는 지금처럼 버전으로 공유한다.
- bindings를 고친 브랜치는 키가 달라져 자동으로 자기 산출물을 갖는다. framework 소스는 패키지가 아니라
  workspace/ProjectReference로 소비되므로 캐시 대상이 아니다.
- 정리: `scripts/local-package/cache-prune.sh --keep 5`가 최근 5개 키만 남긴다.

### 4.2 명령 하나로 — `scripts/dev/work.sh`

절차를 잊지 않도록 단계마다 명령 하나로 묶는다. 감독자와 사람 모두 이 명령으로만 작업을 시작·제출·종료한다.

| 명령 | 하는 일 |
|---|---|
| `work.sh start "<제목>" --area <area> --kind <kind> [--body <파일>]` | Issue 생성(라벨·milestone `1.0`·Project `Todo`) → 브랜치 `<area>/<번호>-<slug>` → worktree `~/project/zlink-<slug>` → 로컬 패키지 링크(§4.1) → Project `In progress`. 출력: worktree 경로 |
| `work.sh pr [--body <파일>]` | 현재 worktree 브랜치 push → PR 생성(첫 줄 `Closes #<번호>`, 본문 §5 세 항목) → Project `Review` |
| `work.sh status` | 이 machine의 worktree·브랜치·Issue·PR·CI check 상태 표 |
| `work.sh done` | PR merge(`--merge`) → 원격 브랜치 삭제 → worktree 제거 → Project `Done`(Issue는 `Closes`로 닫힘) |

`start`는 Issue 본문 세 항목(범위/완료 조건/근거)이 비어 있으면 거부한다. codex job에는 `start`가 낸
worktree 경로를 `-C`로 넘긴다.

## 5. PR과 merge

- `gh pr create --base main --head <branch>`; 본문 첫 줄에 `Closes #N`. 제목은 커밋 메시지 규칙과
  같은 `<모듈>: <한 줄 요약>`.
- PR 본문에는 (1) 무엇을 바꿨나, (2) 검증 — 실행한 gate·테스트·측정 티켓과 결과 수치, (3) 남은 것/
  알려진 red. codex 보고서가 있으면 경로(`.artifacts/codex/<job>/summary.md`)를 링크한다.
- CI: PR 이벤트에서 운영 코드 경로 필터로 워크플로우가 돈다(framework .NET/Node는 있음; Core·bindings·
  framework Java/C++는 Issue #로 추적 중). CI가 없는 영역은 PR 본문의 로컬 검증 기록으로 대신한다.
- merge 조건: (a) 감독자(또는 사용자)가 diff를 읽었다, (b) 해당 영역 gate·테스트가 통과했다(PR CI 또는
  로컬 기록), (c) 측정이 필요한 변경은 티켓 큐 3-run 결과가 PR에 있다. flaky로 알려진 job(현재 .NET
  macOS DrainCoordinator, Node Windows Chromium E2E)은 필수 check에서 제외하고 PR 본문에 상태를 적는다.
- merge 방식: `gh pr merge --merge`(merge commit; 브랜치의 커밋 이력을 유지한다). merge 뒤
  `git worktree remove ~/project/zlink-<slug>`와 원격 브랜치 삭제.
- main 보호: GitHub branch protection으로 PR 필수, force-push·삭제 금지. 관리자(사용자·감독자)의 직접
  push는 §6 예외에만 쓴다(`enforce_admins`는 끈다 — 예외 문서 커밋을 위해).

## 6. 예외 — main 직접 커밋을 허용하는 것

- 계획·결정 기록·worklog(`doc/plan/**`), 메모리성 기록(`.artifacts/**`는 gitignore), 릴리스 노트의
  오탈자 수정처럼 **코드·CI·규격에 영향이 없는 문서**. 감독자가 `git commit -- <경로>`로 파일을 지정해
  커밋한다.
- 릴리스 워크플로우 dispatch와 태그 push(태그는 PR 대상이 아니다).
- 그 밖의 모든 것(운영 코드, 테스트, 벤치 runner·집계기, 규격·가이드 문서, CI)은 PR이다.

## 7. 릴리스와의 관계

- 릴리스는 `CONTRIBUTING.ko.md` §9와 `doc/building/release-pipeline.ko.md`대로 태그로 시작한다. 태그는
  milestone의 Issue가 모두 닫힌 main 커밋에 붙인다.
- 릴리스 중 드러난 수정(워크플로우·버전 필드 누락 등)도 PR로 넣는다. 다만 진행 중인 릴리스를 막는
  수정은 사용자 승인 아래 main 직접 커밋을 허용하고, 사후에 Issue를 만들어 기록한다
  (2026-09-09 0.11.0의 사례: `release-dotnet.yml` 검사, HttpClient 버전, Node `repository` 필드).

## 8. 첫 적용 목록 (2026-09-10)

- Milestone `1.0`, 라벨 §2 생성(완료 2026-09-10). Project `ZLink`는 gh 토큰 `project` 권한 뒤 생성.
- `scripts/dev/work.sh`(§4.2)와 패키지 공유 캐시(§4.1)를 첫 PR로 만든다(Issue 등록).
- 열린 작업을 Issue로: framework messaging 성능 P1~P3(언어별), gRPC bench DEALER→ROUTER·ClientServer 행
  추가, Node framework codec `bytes`, Node/Java raw request-window 유실(FB-049/050), C++ framework
  send peer 소실(FB-054), HTTP host bind 실패 전달(FB-053), Unsafe→FFM 잔여 회귀, bindings 1.0에서
  framework의 `MaxMessageSize`/BLOCKY 참조 정리, Core·bindings·Java/C++ PR CI 추가, .NET macOS
  DrainCoordinator hang, Node Windows Chromium E2E hang.
