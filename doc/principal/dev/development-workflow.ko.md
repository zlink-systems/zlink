# 작업 진행 방식 — Issue · 브랜치/worktree · PR

> 적용 시작: 2026-09-10 (framework 0.11.0 릴리스 뒤, 1.0 준비부터). 사용자 결정.
> 이 문서는 "어떤 작업을 어디에 등록하고, 어느 브랜치에서 하고, 어떻게 main에 넣는가"를 소유한다.
> 커밋 메시지·버전·릴리스 절차는 [`CONTRIBUTING.ko.md`](../../../CONTRIBUTING.ko.md) §9, 에이전트
> 운영 규칙과 job의 금지 범위는 [`CONTRIBUTING.ko.md`](../../../CONTRIBUTING.ko.md) §10과
> [`AGENTS.md`](../../../AGENTS.md)가 소유한다(이 문서는 그 규칙을 반복하지 않고 참조만 한다).
> 2026-09-10 codex 리뷰(`.artifacts/codex/workflow-doc-review/summary.md`, 채택표 `adoption.md`) 반영판.

## 1. 한눈에

```
Issue ──> 브랜치 + worktree ──> 작업(사람 또는 codex job) ──> PR ──> CI + 감독자 검증 ──> merge ──> 정리
  │                                                            │
  └── Milestone(릴리스)·Project(보드)로 묶어서 본다              └── 마지막 PR의 "Closes #N"이 Issue를 닫는다
```

`main`은 **PR로만** 바뀐다. 예외는 §6에 모두 적는다(다른 절은 §6을 가리키기만 한다).
브랜치 하나 = Issue 하나 = worktree 하나. Issue 하나에 PR이 여럿일 수 있다(§5).

## 2. 작업 등록 — GitHub Issue

### 2.0 세 가지 일을 구분한다

절차는 **main에 들어갈 변경**을 위한 것이다. 모든 일에 같은 무게를 씌우지 않는다.

| 종류 | Issue | 브랜치·worktree | PR | 보드 |
|---|---|---|---|---|
| **main에 들어갈 변경** | 필요 | 필요 | 필요 | 올린다 |
| **결함 발견** | 필요(고칠 때까지 열어 둔다) | 고칠 때 만든다 | 고칠 때 | 올린다 |
| **버리는 실험·측정** | **만들지 않는다** | 필요하면 임시로 만들되 보드에 올리지 않는다 | **만들지 않는다** | 올리지 않는다 |

**실험 결과가 남길 만하면 Issue 없이 그대로 PR을 연다.** main 보호가 요구하는 것은 PR이지 Issue가 아니다.
그 PR 본문에 보고서 경로와 측정값을 근거로 적으면 추적에 충분하다. Issue를 만드는 경우는 따로 있다 —
여러 세션·여러 사람에 걸치는 일, 지금 고치지 않고 나중에 할 일, 릴리스 범위 판단에 들어가야 할 일이다.
즉 **실험 → (좋으면) PR**이 기본 경로이고, Issue는 그 일을 남에게 넘기거나 미뤄야 할 때 만든다.
**Issue가 필요해지는 시점은 PR을 올릴 때다** — 먼저 만들 이유가 없다. worktree에서 바로 시작하고,
PR 단계에서 추적이 필요하면 그때 등록해 연결한다. 미리 만드는 경우는 둘뿐이다: 오래 걸려서 진행 중에도
보드에서 보여야 하는 일, 그리고 다른 사람이 이어받을 일.

실험은 `.artifacts/codex/<이름>/`에 브리프와 보고서만 남긴다. 코드를 고쳐야 하면 worktree를 만들되
Issue·PR·보드 없이 쓰고, 끝나면 [§4.3의 worktree 정리](#43-그-밖의-개발-스크립트)로 지운다. 실험에서
**결함이 나오면 그때 Issue를 만든다** — 실험 자체를 Issue로 만들지 않는다.

판단 기준은 하나다: **이 작업의 결과가 main에 남는가.** 남지 않으면 절차를 씌우지 않는다.

### 2.1 Issue 작성

- 모든 작업은 시작 전에 Issue로 등록한다. 제목은 한 줄 결과("무엇이 어떻게 되어야 한다"), 본문은
  **범위 / 완료 조건 / 근거** 세 항목이며 각 항목이 비어 있으면 안 된다. 근거는 결정 기록의 번호
  (`FB-nnn`·`D-nnn`)와 **파일 경로**(`doc/plan/fw-bench-worklog/decisions.ko.md#fb-056`처럼 절 anchor까지),
  측정값, 보고서 경로다.
- 라벨은 두 축만 쓴다.
  - `area:` `core` · `bindings` · `framework-dotnet` · `framework-java` · `framework-node` · `framework-cpp` · `bench` · `ci` · `docs`
  - `kind:` `bug` · `perf` · `feature` · `chore`
- 한 Issue는 원인 하나·결과 하나다. 여러 언어에 같은 수정이 필요하면 언어별 Issue로 나누고 본문에서
  서로 링크한다. 진단과 수정처럼 **원인이 같은 후속 단계는 같은 Issue**에 둔다(PR을 나눈다, §5).
- 결정 기록(`doc/plan/**/decisions*.md`)이 근거이고 Issue는 그것을 꺼낸 "할 일"이다. 상호 참조 형식:
  Issue 본문에 `근거: decisions.ko.md#fb-056`, 결정 기록의 해당 항목 제목에 `(Issue #7)`. Issue를 만들 때
  결정 기록 쪽 역링크를 같은 커밋에서 넣는다(§6 예외 문서).

## 3. 묶음 — Milestone과 Project

- **Milestone** = 릴리스. `1.0`(Core 1.0 · bindings 1.0.0 · framework 1.0.0)처럼 릴리스 이름으로 만들고,
  그 릴리스에 들어가야 하는 Issue에 붙인다. 릴리스 태그 전 조건은 "milestone의 Issue가 모두 **merge된
  PR로** 닫혔고, 태그할 main 커밋이 그 PR들을 모두 포함한다"이다(수동 close는 세지 않는다; `work.sh status
  --milestone 1.0`이 이 두 조건을 검사한다). milestone 이름은 `work.sh start --milestone`으로 주고,
  기본값은 `scripts/dev/work.conf`의 `current_milestone`이다(릴리스 뒤 한 곳만 바꾼다).
- **Project** = 보드 `ZLink`(https://github.com/users/zlink-systems/projects/1, 저장소 Projects 탭에도 연결).
  보드의 행은 **Issue만**이다(PR은 Issue의 연결 정보로 본다). Status `Todo → In progress → Review → Done`,
  필드 `area`(라벨과 같은 값, 자동 채움), `runner`(`astra` / `sol` / `direct` / 비움; 선택). 상태 전환의
  주체는 **`work.sh`와 동등한 Windows `work.ps1` 진입점**이다(GitHub 내장 workflow는 같은 필드를
  건드리지 않게 끈다). Project 갱신 실패는
  경고로 남기고 작업을 막지 않는다(§4.2).
- 라벨은 §2의 두 축으로 끝내고 상태·runner는 Project 필드로만 관리한다.

## 4. 브랜치와 worktree

- 브랜치 이름: `<area>/<issue번호>-<slug>` — 예 `framework-java/6-mesh-pump`, `bench/13-dealer-rows`.
- worktree 경로: `~/project/zlink-<issue번호>-<slug>` (번호를 넣어 같은 slug 충돌을 막는다).
  `git worktree add <경로> -b <branch> origin/main` — 시작 전에 `git fetch`로 최신 `origin/main`에서 분기한다.
  같은 브랜치를 두 worktree에서 열지 않는다.
- codex job에는 worktree 경로를 `-C`로 주고 **그 브랜치에만** 커밋하게 한다(commit 위임은 브리프에
  명시한 경우만; 위임하지 않은 job의 완료물은 diff와 보고서다). push·PR·merge는 감독자가 한다. job이
  만지지 못하는 경로는 `CONTRIBUTING.ko.md` §10이 정본이다.
- main을 따라잡을 때는 `git fetch && git merge origin/main`(rebase는 공유 브랜치에서 쓰지 않는다).

### 4.1 로컬 패키지 공유 캐시 (content-addressed)

binding 로컬 패키지(nuget `Zlink.*`, npm `@zlink-systems/zlink`, maven `systems.zlink:zlink*`, C++ `install/zlink-cpp`)는
입력이 같으면 결과가 같으므로 해시로 공유한다(vcpkg binary cache·Conan cache와 같은 원리). 규칙:

- **키** = `sha256(bindings/ 트리 해시 ‖ 언어별 bindings/<language>/VERSION 값 map ‖ Core 버전 ‖ scripts/local-package/ 트리 해시 ‖ 플랫폼
  `<os>-<arch>` ‖ 도구 버전 id)` 앞 16자리. 도구 버전 id는 `build-wsl.sh`가 사용하는 컴파일러·SDK·Node·JDK
  버전을 한 줄로 합친 값이다(스크립트가 출력한다).
- **깨끗한 트리에서만 공유**: `bindings/`·`scripts/local-package/`에 staged·unstaged·untracked 변경이 있으면
  공유 캐시를 쓰지 않고 worktree 전용 디렉터리(`.artifacts/wsl-private/`)에 빌드한다. dirty 산출물은 절대
  공유 키로 게시하지 않는다.
- **위치와 게시**: `~/.cache/zlink/packages/<키>/`. 빌드는 `<키>.staging-<pid>/`에서 하고 검증(각 언어 패키지의
  존재·digest 기록)까지 끝난 뒤 `rename`으로 원자 게시한다. 게시된 키는 불변이다. 동시 빌드는 `<키>.lock`
  (`flock`)으로 단일 작성자를 보장한다. `.complete`에는 언어별 산출물 목록과 digest를 적는다.
- **공유 범위는 binding 패키지뿐**: worktree의 `.artifacts/wsl/`은 worktree별 쓰기 디렉터리로 두고, 그 안의
  binding 패키지 파일만 캐시로의 symlink다. framework가 만드는 패키지(`Zlink.HttpClient`, `@zlink-systems/http-client`
  등)와 빌드 트리는 worktree별이다. Core release prefix `~/.cache/zlink/core/<ver>/<플랫폼>`은 지금처럼 공유한다.
- **hit 경로도 검증한다**: 링크만 하는 경우에도 `sync-version.py --check`와 `build-wsl.sh --verify-versions`를
  돌린다(miss 경로와 같은 검사). 소비 확인: .NET은 gate의 package digest별 `NUGET_PACKAGES` 방식을 그대로 써
  같은 버전·다른 내용의 패키지가 섞이지 않게 한다. npm·Maven도 digest 기준으로 확인한다.
- **정리**: `scripts/local-package/cache-prune.sh --keep 5`는 최근 5개를 남기되, 존재하는 worktree가 링크한
  키와 baseline worktree의 키는 지우지 않는다. `work.sh done`은 링크만 지우고 캐시는 건드리지 않는다.
- 문서 작업처럼 패키지가 필요 없는 Issue는 `work.sh start --no-packages`로 시작한다.

### 4.2 명령 하나로 — `scripts/dev/work.sh` / Windows `scripts/dev/work.ps1`

절차를 잊지 않도록 단계마다 명령 하나로 묶는다. 사람도 감독자도 이 명령으로만 시작·제출·종료한다.
**모든 명령은 재실행해도 안전하다**: 기존 Issue/branch/worktree/PR 상태를 먼저 조회해 끝난 단계는
건너뛰고 미완료 단계부터 이어 가며, 실패하면 만들어진 ID·URL·경로와 재개 명령을 출력한다.

| 명령 | 하는 일 |
|---|---|
| `work.sh start "<제목>" --area <area> --kind <kind> --body <파일> [--milestone <이름>] [--no-packages]` | Issue 생성(본문 세 항목이 비어 있으면 거부) → 라벨·milestone → 브랜치 → worktree `~/project/zlink-<번호>-<slug>` → 로컬 패키지 준비(§4.1) → Project `In progress`. 출력: Issue 번호와 worktree 경로 |
| `work.sh start --issue <N> [--no-packages]` | **이미 있는 Issue**로 시작하거나 이어간다(중복 Issue를 만들지 않는다). 브랜치·worktree가 있으면 재사용 |
| `work.sh pr --body <파일> [--closes\|--refs]` | 제목·본문 세 항목·Issue 번호·base=main 검증 → push → PR 생성(있으면 갱신). 마지막 PR만 `Closes #N`, 중간 PR은 `Refs #N`(기본은 `--refs`; 완료 조건을 다 채웠을 때 `--closes`) → Project `Review` |
| `work.sh status [--milestone <이름>]` | 이 machine의 worktree·브랜치·Issue·PR·CI check 표. `--milestone`은 §3의 릴리스 조건(merge된 PR로 닫힘 + main 포함)을 검사 |
| `work.sh done --verified <sha>` | PR HEAD가 `<sha>`(감독자가 diff를 읽고 검증한 커밋)와 같을 때만 `gh pr merge --merge --match-head-commit <sha>` → 원격 브랜치 삭제 → worktree 제거(dirty·미push 변경이 있으면 거부) → Project `Done`. `Refs` PR이면 Issue는 열린 채 두고 worktree는 유지한다 |

- Project·milestone 갱신은 best-effort다: 권한이 없거나 통신이 실패하면 경고를 내고 로컬 단계는 계속한다.
  `status`는 로컬 정보를 항상 보여 준다.
- `work.sh`가 생기기 전(§8)의 수동 절차는 §8에 적힌 명령 목록이다.
- Windows에서는 같은 하위 명령과 옵션을 `powershell -File scripts/dev/work.ps1 ...`로 실행한다.

### 4.3 그 밖의 개발 스크립트

`work.sh`는 Issue 하나의 흐름만 맡는다. 그 바깥에서 반복되는 일은 다음 스크립트가 맡는다.
있는 것과 없는 것을 구분해 적는다 — **없는 스크립트를 문서가 있는 것처럼 적지 않는다.**

| 스크립트 | 하는 일 | 상태 |
|---|---|---|
| `scripts/dev/work.sh` | Issue → 브랜치·worktree → PR → merge·정리 (§4.2) | 있음 |
| `scripts/local-package/package-cache.py`·`cache-prune.sh` | binding 로컬 패키지 공유 캐시와 정리 (§4.1) | 있음 |
| `scripts/perf/perf-ticket.sh`·`perf-queue-runner.sh` | 모든 측정을 티켓 하나씩 직렬로 실행 | 있음 |
| `scripts/gate/*.sh` | 언어별 게이트 | 있음 |
| `scripts/dev/job.sh` | codex sub-agent job의 시작·상태·3분 주기 감시·종료. 시작 직후 로그를 검사해 잘못된 모델 id나 인증 실패를 즉시 알린다. 종료는 기록한 pid로만 한다 | 있음 |
| `scripts/dev/worktree-sweep.sh` | 방치된 worktree를 안전 기준(미커밋·미push·main 포함·실행 중 job)으로 판정해 정리 | 있음 |
| `scripts/dev/session-setup.sh` | 세션 시작 시 벤치 포트 예약·tmpfs 여유·측정 큐·로컬 패키지 상태를 한 번에 맞춘다 | 있음 |
| `scripts/dev/release-check.sh` | 태그 전에 버전 동기화·릴리스 노트·패키지 메타데이터·배포 대상을 검사 | 있음 |
| 벤치 결과 비교 도구 | 측정 두 벌을 시나리오 × payload 표(처리량·지연·비율·변화)로 낸다 | Issue #37 |
| `scripts/dev/ci-watch.sh` | CI 감시자를 하나로 제한하고 10분 주기로 확인한다(GitHub API 한도) | 있음 |

원칙 셋을 이 스크립트들에 공통으로 적용한다.

- **재실행 안전**: 같은 명령을 다시 실행해도 상태가 어긋나지 않는다.
- **조용한 실패 금지**: 배경 프로세스를 띄우는 스크립트는 시작 직후 살아 있는지 확인하고, 죽었으면 로그의 마지막 오류를 사람에게 보여 준다.
- **이름으로 죽인다**: 프로세스 종료는 기록한 pid로만 한다. `pkill -f`처럼 패턴으로 찾으면 호출자 셸까지 죽는다.

## 5. PR과 merge

- PR 제목은 `<모듈>: <한 줄 요약>`. 본문 첫 줄은 `Closes #N`(완료) 또는 `Refs #N`(부분). 본문에는
  (1) 무엇을 바꿨나, (2) **검증 — 실행 명령, 대상 SHA, 패키지 digest, 핵심 결과 수치(표), 남은 실패**를
  본문에 직접 싣는다. 로컬 보고서 경로(`.artifacts/codex/<job>/summary.md`)는 보조 위치이며 gitignore라
  다른 machine에서 보이지 않으므로, 판정에 쓴 표는 PR 본문(또는 `doc/plan/**` 기록)에 옮겨 적는다.
- CI: PR 이벤트에서 운영 코드 경로 필터로 워크플로우가 돈다. 현재 있는 것은 framework .NET·Node이고,
  Core·bindings·framework Java/C++는 Issue #16이다. 문서·벤치·샘플·scenario E2E·`VERSION`·`scripts/local-package`
  변경에는 PR CI가 없으므로 PR 본문의 로컬 검증 기록(§5 두 번째 항목)이 대신한다. 문서 PR은 `mkdocs build --strict`
  결과를 적는다.
- merge 조건: (a) 감독자(또는 사용자)가 **PR HEAD의 diff**를 읽었다(`done --verified <sha>`), (b) 영역의
  gate·테스트가 통과했다(PR CI 또는 본문 기록), (c) 측정이 필요한 변경은 티켓 큐 3-run 결과가 본문에 있다.
  pending·cancelled·미실행 check는 PASS가 아니다.
- flaky 처리는 **test 단위**다: 알려진 flaky test는 Issue(현재 #17 .NET macOS DrainCoordinator, #18 Node
  Windows Chromium E2E)로 추적하고, 그 test가 포함된 job이 그 test만으로 실패한 경우에만 PR 본문에 근거를
  적고 넘어간다. job 전체를 면제하지 않는다. 필수 status check는 지금 비어 있고, Issue #16의 PR CI가
  갖춰진 뒤 job 이름 기준으로 지정한다(paths 밖 PR이 대기 상태에 빠지지 않게 "skipped도 통과"인 집계 job만 필수로 둔다).
- merge 방식: `gh pr merge --merge`(merge commit). merge 뒤 원격 브랜치 삭제와 worktree 제거는 `done`이 한다.
- main 보호: PR 필수, force-push·삭제 금지. 관리자(사용자·감독자)의 직접 push는 §6에만 쓴다
  (`enforce_admins` off).

## 6. 예외 — main 직접 커밋을 허용하는 것 (유일한 목록)

1. 코드·CI·규격에 영향이 없는 **기록 문서**: `doc/plan/**`(계획·결정 기록·worklog·결과), 릴리스 노트 오탈자.
   감독자가 `git commit -- <경로>`로 파일을 지정해 커밋한다.
2. 릴리스 워크플로우 dispatch와 태그 push(태그는 PR 대상이 아니다).
3. **진행 중인 릴리스를 막는 수정**(워크플로우 검사·버전 필드처럼 그 릴리스에서만 문제가 되는 것)은
   사용자 승인 아래 직접 커밋할 수 있고, 사후에 Issue를 만들어 기록한다(0.11.0의 사례: `release-dotnet.yml`
   검사, HttpClient 버전, Node `repository` 필드).

그 밖의 모든 것(운영 코드, 테스트, 벤치 runner·집계기, 규격·가이드 문서, CI, `scripts/**`)은 PR이다.

## 7. 릴리스와의 관계

- 릴리스는 `CONTRIBUTING.ko.md` §9와 `doc/building/release-pipeline.ko.md`대로 태그로 시작한다. 태그 대상
  커밋은 §3의 milestone 조건을 만족하는 main 커밋이다(`work.sh status --milestone`).
- 릴리스 중 드러난 수정은 §6-3에 따른다.

## 8. 첫 적용 (2026-09-10) — `work.sh`가 생기기 전의 수동 절차

- 완료: Milestone `1.0`, 라벨 §2, Project `ZLink`(Issue #5~#20 등록).
- `work.sh`(§4.2)와 공유 캐시(§4.1)는 Issue #20의 PR로 만든다. 그 PR이 merge되기 전까지는 아래 명령을
  손으로 실행한다(순서와 내용은 §4.2와 같다).

```bash
# 시작 (기존 Issue N)
git fetch origin
git worktree add ~/project/zlink-N-<slug> -b <area>/N-<slug> origin/main
mkdir -p ~/project/zlink-N-<slug>/.artifacts && ln -s ~/project/zlink/.artifacts/wsl ~/project/zlink-N-<slug>/.artifacts/wsl   # 임시: 공유 캐시 전까지
gh project item-edit --project-id PVT_kwHOErOLTs4Bi-bl --id <item> --field-id <Status> --single-select-option-id <In progress>
# 제출
git push -u origin <area>/N-<slug>
gh pr create --base main --head <area>/N-<slug> --title "<모듈>: <요약>" --body-file <본문>   # 첫 줄 Refs #N 또는 Closes #N
# 종료 (검증한 SHA로)
gh pr merge <PR> --merge --match-head-commit <sha>
git push origin --delete <area>/N-<slug>
git worktree remove ~/project/zlink-N-<slug>
```
