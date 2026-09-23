# Agent Guidelines

이 파일은 저장소 전체에 적용하는 최소 공통 규칙이다. 특정 디렉터리의 상세 규칙은 그
디렉터리의 `AGENTS.md`가 소유한다. `CLAUDE.md`는 이 파일을 단일 진입점으로 참조한다.
사람용 절차와 위치(빌드, gate, 성능 판정, 릴리스, 판정 기록)는 [`CONTRIBUTING.ko.md`](./CONTRIBUTING.ko.md)가 소유한다.

## 1. 작업 시작과 변경 보호

- 작업 전에 `git branch --show-current`를 확인하고, 사용자가 지정한 branch에서만 수정, commit과
  push를 수행한다. 어느 branch인지 분명하지 않으면 먼저 묻는다.
  작업 단위는 Issue 브랜치(`<area>/<issue번호>-<slug>`)와 worktree이며, main은 PR로만 바뀐다
  (`doc/principal/dev/development-workflow.ko.md`). 간단한 작업은 worktree 없이 main에서 한다 — 무엇이
  간단한 작업인지는 그 문서 §6이 정한다.
- 작업 상태가 바뀔 때마다(시작, 리뷰 대기, 머지, 보류) [Project 보드](https://github.com/users/zlink-systems/projects/1/views/10)의
  상태를 바로 갱신한다. 새 Issue는 만들 때 보드에 올린다.
- worktree는 Windows에서는 `D:\worktree\<이름>`, WSL에서는 `/home/hep7/worktree/<이름>` 아래에 만든다.
  WSL 검증 사본(rsync 복사본)도 같은 WSL 디렉터리 아래에 두고, 작업이 끝나면 지운다 — 흩어진 사본이
  쌓여 WSL 디스크 이미지가 D:를 가득 채운 적이 있다(2026-09-23).
- 머지되거나 다른 브랜치에 합쳐져 끝난 worktree는 바로 지운다(`git worktree remove`, WSL 사본 포함).
- 변경이 있는 worktree에서는 먼저 범위를 보고하며, 승인 없이 branch 전환, `reset`, `restore`,
  강제 checkout 또는 삭제를 하지 않는다.
- 기존 변경과 untracked 파일은 사용자 작업이다. 요청 범위 밖의 변경을 수정하거나 정리하지 않는다.
- branch 생성·전환·merge와 commit·push는 사용자가 명시적으로 요청한 경우에만 수행한다.

## 2. 효율적인 조사와 실행

- 먼저 목표, 수정 범위, 완료 조건과 필요한 검증을 정한다. 사용자가 요구하지 않은 계획서,
  ledger, gap 문서를 만들지 않는다.
- 파일 검색은 `rg`와 `rg --files`를 우선한다. 관련 symbol과 호출 경로부터 좁히고, 근거 없이
  디렉터리 전체나 대형 파일 여러 개를 읽지 않는다.
- 이미 확인한 repository 구조와 실행 결과를 같은 작업에서 다시 조사하지 않는다. 재시도할 때는
  마지막 실패와 변경된 경계만 확인한다.
- 명령 출력은 필요한 범위로 제한한다. 전체 log 대신 첫 실패, 관련 구간과 최종 요약을 읽는다.
  전체 log가 필요하면 파일에 보관하고 필요한 부분만 조회한다.
- 독립 범위가 분명하지 않으면 agent를 추가하지 않는다. 같은 파일이나 같은 원인을 여러 agent가
  중복 조사하지 않는다.
- 가능한 가장 작은 검증부터 실행한다. 관련 test → subsystem suite → 최종 전체 gate 순서로
  확장하며, 원인 변화 없이 같은 전체 gate를 반복하지 않는다.
- lint, format, rename 같은 기계적 작업에는 사용할 수 있는 가장 가벼운 도구나 model을 사용한다.

- 이 머신에 이미 있는 검증 자산(교차 툴체인 `g++-11`·`g++-13`, vcpkg Boost, emsdk,
  Playwright Chromium)과 반복해서 밟은 함정은
  [`doc/principal/dev/workspace-notes.ko.md`](./doc/principal/dev/workspace-notes.ko.md)에
  있다. **로컬에서 재현할 수 있는 것을 CI 왕복으로 확인하지 않는다.**

### 2.1 Sub-agent model·추론 레벨

**감독자가 직접 하는 일은 넷이다: 감독, 리뷰, 스펙 작성, 자잘한 작업.** 리뷰는 코드와 가이드의 diff를
감독자가 직접 읽고 판정하는 것이며, sub-agent 리뷰와 검사 통과는 판정의 입력일 뿐이다. 스펙은 감독자가
쓰고 sol 리뷰를 거친다([5.1](#51-스펙-개정-절차)). 자잘한 작업은 main에서 한다([1](#1-작업-시작과-변경-보호)) — 다만 간단해도 시간이 걸리는 작업은
`luna`나 `sonnet`에게 맡긴다.
그 밖의 구현·조사·검증 실행은 sub-agent에게 맡긴다.

**모델은 작업의 모호성·중대성으로 고르고, 추론 레벨은 작업 난이도로 고른다. 둘은 별개다.**
약한 모델을 높은 레벨로 보상하려 하지 않는다 — 그 경우 모델 등급을 올리는 쪽이 맞다.
근거: 최신 모델의 낮은 레벨이 이전 세대의 high를 넘는 경우가 많고, 비용 캐스케이드를 짜기
전에 "가장 유능한 모델을 낮은 레벨로" 돌려보는 것이 먼저다.

#### 추론 레벨

레벨은 `none`·`low`·`medium`·`high`·`xhigh`·`max` 여섯이고, **`medium`이 시작점이다** — GPT-6 Sol·Luna와
Claude Opus 5.5의 기본값이며 최신 모델의 낮은 레벨이 이전 세대의 높은 레벨을 넘는 경우가 많다.

- 올리는 기준은 **측정된 실패**다. 지레 올리지 않는다. 높은 레벨이 늘 더 나은 결과를 내지는 않는다.
- 올려야 할 작업의 성격: 깊은 의존성 추적, 모호한 진단, 아키텍처 트레이드오프, 보안·릴리스
  최종 리뷰.
- `none`은 쓰지 않는다(sub-agent 작업은 모두 추론이 필요하다).

#### Codex (`codex exec -m <model id> -c model_reasoning_effort=<level>`)

GPT-6는 세 등급이다(OpenAI 발표 2026-09-22, ChatGPT 계정 실측 2026-09-23). Terra는 이 세대에 없다 —
Luna가 이전 Terra의 일 대부분을 맡고 Sol이 이전 Terra보다 싸다.

| 모델 | 모델 id | 입력 / 캐시 입력 / 출력 (per 1M) | 쓰는 일 | 시작 레벨 |
|------|---------|----------------------------------|---------|-----------|
| `sol` | `gpt-6-sol` | $2 / $0.20 / $10 | **기본.** 구현, 리뷰, 진단, 여러 파일에 걸친 설계 판단, 계약·수명·에러 경로가 걸린 구현 | `medium` |
| `luna` | `gpt-6-luna` | $0.10 / $0.01 / $0.50 | 정해진 반복 작업: 검증 실행, 문서 대조, 전수 수집, 정해진 패턴 적용, 단일 파일 수정. 여러 파일에 걸친 코딩에는 쓰지 않는다 | `medium` |
| `astra` | `gpt-6-astra` | $10 / $1 / $50 | 큰 변경의 최종 읽기 전용 리뷰 한 번(아래) | `low` |

- **`sol`이 기본이다.** 기계적이고 결과를 바로 검증할 수 있는 일만 `luna`로 내린다. 레벨은 둘 다
  `medium`에서 시작해 측정된 실패가 있을 때만 올린다(Luna는 `max`까지 올리면 이전 Terra 수준이다).
- **`astra`는 구현에 쓰지 않는다.** 가장 비싼 등급이고 272K 토큰을 넘으면 요금제가 또 바뀐다.
  `none`을 지원하지 않으며 `low`에서 시작한다 — Astra의 `low`가 Sol의 `high`를 넘는 경우가 많다.
  `sol`이 막힌 작업은 브리프를 좁혀 다시 `sol`로 보내거나 감독자가 직접 한다. 예외는 **큰 변경의
  최종 읽기 전용 리뷰 한 번**이다(2026-09-20 #736 실측: sol 리뷰 4회가 승인한 diff에서 astra가
  산출물을 메모리에서 변환해 경계 입력을 재현하는 방식으로 실행 가능한 결함 9건을 찾았다 — 입력 145K
  토큰). sol 리뷰가 먼저 승인한 뒤에만 넣고, 브리프에 "sol이 놓친 것"만 찾으라고 적는다.
- **한도가 차면** 먼저 `luna`로 옮길 수 있는 일을 옮기고, 그래도 막히면 Claude sub-agent로 대체한다.
- **캐시된 입력은 10배 싸다.** 연관된 job은 브리프 앞부분(원칙·필독 문서 목록)을 동일하게
  유지해 prefix가 재사용되게 한다.

#### Claude (`Agent` 도구의 `model`)

Claude도 추론 레벨을 정할 수 있으나 **호출 시점이 아니라 에이전트 정의에서** 정한다 —
`Agent` 도구 호출 파라미터에는 `model`만 있고, 그 에이전트 타입의 model·추론 레벨·tools는
`.claude/agents/*.md` frontmatter가 소유한다. 정의 파일이 없으면 세션 기본값을 상속한다.

| 모델 | 쓰는 일 |
|------|---------|
| `fable` | **감독 레벨 전용.** sub-agent에는 사용하지 않는다 |
| `opus` | 감독 레벨, 그리고 `sonnet`으로 안 되는 sub-agent 작업: 판단이 필요한 구현, 코드 리뷰, 원인이 좁혀진 진단, 코드베이스 전반의 이전·감사 |
| `sonnet` | **sub-agent 기본값.** 변경 지점이 명확한 구현, 정해진 절차의 실행, 빌드·테스트 반복, 문서 수정, 기계적 수집과 정찰 |

- **`sonnet`에서 시작하고, `sonnet`이 실패했거나 판단이 필요한 작업만 `opus`로 올린다.**
- **Opus 5.5는 대부분의 작업에서 Fable 5.1 수준이고 Opus 5보다 40% 싸다**(입력 $4, 출력 $20, 캐시 읽기
  $0.20 per 1M; 기본 레벨 `medium`). 길고 넓은 작업(코드베이스 전반의 이전·감사)과 sub-agent
  위임·자기 검증에 강하므로 감독 레벨에서도 쓴다.
- **`fable`은 sub-agent에 지정하지 않는다.** 예외는 특별한 이슈가 있을 때뿐이며, 그 경우 사유를 결정
  기록에 남긴다.
- **사용하는 모델은 위 세 개뿐이다.** `haiku`를 비롯한 다른 모델은 쓰지 않는다.

#### 어느 도구를 쓸 것인가

**Sub-agent는 codex를 우선 사용한다.** codex를 쓸 수 있으면 sub-agent는 codex다. Claude
sub-agent는 codex에 이슈가 있을 때 쓴다 — 쿼터 소진, 콘텐츠 필터로 job이 죽음, 반복 실패,
또는 codex가 접근할 수 없는 작업. Claude로 대체했으면 그 사유를 결정 기록이나 작업 로그에
남긴다. Claude의 세션·주간 한도는 codex보다 훨씬 빨리 닳아 작업자가 한꺼번에 멈춘다.

- 실행은 `codex exec -m <model id> -c model_reasoning_effort=<level> -C <worktree> …`로 하고,
  이어서 할 때는 `codex exec resume <session id> "<후속 지시>"`로 같은 세션을 잇는다 — 새
  세션을 열면 읽은 것을 다시 읽는다.

#### 공통 규칙

- 큰 작업은 단계로 나눠 정찰·수집을 가벼운 agent에 먼저 맡기고, 무거운 model은 판단과 설계가
  필요한 단계에만 투입한다.
- 지정하지 않으면 세션 설정을 상속한다. 기계적 작업이면 명시적으로 낮춘다.
- 이미 실행 중인 agent는 model을 바꾸려고 중단·재투입하지 않는다. 재작업 비용이 더 크다.
- **투입한 sub-agent는 3분 단위로 동작을 확인한다.** 살아 있는지, 무엇을 건드리고 있는지,
  로그가 자라고 있는지를 본다. 멈췄거나 엉뚱한 곳을 파고 있으면 그때 바로잡는다 — 끝날 때까지
  기다렸다가 결과만 보면 되돌리는 비용이 커진다.
- **동시에 몇 개를 돌릴지는 고정 숫자가 아니라 작업 순서와 남은 자원으로 정한다.**
  - 먼저 **작업 순서**를 본다. 서로 다른 원인·다른 모듈이면 함께 돌린다. 앞선 job의 결과를
    입력으로 쓰는 job은 기다린다. 같은 파일을 고칠 job을 나란히 돌리지 않는다. 다른 job을
    막고 있는 것(예: 벤치를 못 돌리게 하는 결함)을 먼저 넣는다.
  - 그다음 **남은 자원**을 본다. 시작 전에 `nproc`, `/proc/loadavg`, `free -g`를 확인한다.
    job 하나가 빌드·테스트로 코어 여럿을 쓰므로, **load average가 코어 수의 절반을 넘으면
    새로 넣지 않는다.** 메모리도 job마다 넉넉히 잡아 남은 양으로 나눈다.
  - 자원을 통째로 쓰는 작업 옆에는 아무것도 넣지 않는다 — Core LTO 빌드, gate, perf 측정이
    그렇다. 이들은 하나씩 돌린다.
  - 돌리는 동안에도 load를 다시 본다. 올라가면 추가 투입을 멈추고, job들이 서로 느려지면
    줄인다.
- **sub-agent는 스펙 문서를 수정하지 않는다.** 스펙·정책 개정은 감독자가 직접 한다.
- sub-agent가 낸 발견은 인용한 사양·코드를 감독자가 직접 열어 재검증한 뒤에만 채택하고,
  채택/기각 사유를 결정 기록에 남긴다.
- 위 실측 근거는 GPT-5.5 세대 기준이다. 우리 job의 결과가 이와 다르면 관찰을 기록하고 기준을
  조정한다 — 이 표는 출발점이지 고정값이 아니다.

## 3. 구현 원칙

- 기존 public API, 표준 호출 경로와 abstraction을 먼저 사용한다. 같은 의미의 helper, DTO,
  adapter나 우회 경로를 추가하지 않는다.
- 실패 지점을 우회하지 말고 책임을 소유한 모듈에서 원인을 수정한다. codec, transport, retry,
  lifecycle과 registry 결정을 호출자나 sample로 밀어내지 않는다.
- 새 public API가 필요해 보이면 기존 계약 근거를 먼저 확인한다. 계약이 없으면 구현 전에
  설계 변경으로 분리해 사용자에게 보고한다.
- sample은 public 사용 예시다. test를 통과시키기 위해 sample에 internal helper, raw frame 해석,
  private policy 또는 임시 codec을 넣지 않는다.
- Framework message는 기본 typed JSON serializer 경로를 사용한다. 메시지별 codec 등록 API나
  호출부의 수동 encode/decode로 내부 연결 문제를 우회하지 않는다.
- 비자명한 설계는 최소 두 대안을 비교하되, 요청하지 않은 장문의 설계 문서를 만들지 않는다.
  인터페이스를 단순하게 유지하고 자료구조와 protocol 결정은 소유 모듈 안에 숨긴다.
- **계층 소유권(필수).** 어떤 결정(연결 선택·교체, reconnect, handover 수렴, completion drain,
  DONTWAIT 재제출, errno 분류, reply 라우팅, 재전송)을 코드에 넣기 전에 그 결정을 소유한 계층을
  spec 조항으로 먼저 확인한다. Core·binding이 소유한 결정을 Framework에서 다시 구현하거나 같은
  사실을 두 곳의 상태로 유지하지 않는다. 하위 계층이 spec과 다르게 동작하면 상위에서 보상하지
  말고 하위 계층의 버그로 보고한다(공개 API repro 포함).
- **금지 패턴.** 다음은 원인 수정이 아니라 우회이므로 하지 않는다: timeout·budget·retry 횟수 증가,
  "안전을 위한" 재시도나 순서 변경, monitor event로 만든 별도 pair·generation 상태로 수신 frame을
  걸러내기, 같은 socket에 두 번째 poller, 예외를 삼키는 catch-all, 실패를 없애기 위한 fixture 조건
  완화, 구현에 맞춘 assertion 변경.
- **교차언어 대조(필수).** Framework runtime 동작을 바꾸기 전에 같은 상황을 다른 언어 구현이 어떻게
  처리하는지 확인한다. 한 언어만 변경이 필요하면 그 이유가 구조적 차이인지 다른 root cause의
  증상인지 완료 보고에 적는다.
- **원칙이 단순해지는 근본 수정(필수).** 여기서 "단순"은 변경량이 아니라 **결과 설계의 규칙 수**다. 수정 뒤에 사실 하나에
  소유자 하나, 결정 하나에 규칙 하나, 특수 case·예외 경로·조건 분기가 줄어들어야 한다. 규칙을 줄이기 위한 큰 diff는
  허용하고, 규칙을 하나 더 얹는 작은 diff(새 상태·타이머·인덱스·헬퍼 계층·옵션·"이 경우만" 분기)는 금지한다. 두 대안이 있으면
  설명해야 할 규칙이 적은 쪽을 고르고, 완료 보고에 "수정 전/후 규칙 수"를 한 줄로 적는다. 같은 규칙·상태·헬퍼·매핑표를 두 곳에
  두지 않는다(중복 금지): 이미 있는 소유자를 재사용하고, 새로 만들어야 하면 기존 것을 그 자리로 옮겨 하나만 남긴다.
- **진단 먼저, 구현은 승인 뒤.** Framework runtime 수정은 두 단계로 한다. 1단계는 코드 변경 없이
  원인 `file:line`, 소유 계층과 spec 조항, 교차언어 대조, 변경 분류(A 계약 적응 / B 기존 결함 /
  C 우회 / D spec gap)를 보고한다. 감독이 A 또는 B로 승인한 뒤에만 2단계 구현을 시작한다.
- POSDDD 원칙 문서
  [`doc/principal/dev/posddd.ko.md`](./doc/principal/dev/posddd.ko.md)와
  [`doc/principal/dev/zlink-system-design-principles.ko.md`](./doc/principal/dev/zlink-system-design-principles.ko.md)는
  요청과 무관하게 runtime 변경 시 항상 적용한다.

## 4. 검증과 완료 보고

- 수정 중에는 관련 test만 실행한다. 전체 test, E2E, sample과 benchmark는 사용자 요청 또는
  최종 검증 필요성이 있을 때 한 번 실행한다.
- 첫 실제 실패에서 원인을 분리한다. unrelated failure를 임의로 고치거나 expectation을 낮추지 않는다.
- **CI는 마지막에 한 번이다.** 로컬에서 확인할 수 있는 것(빌드, unit·contract test, 스크립트 검사,
  workflow 문법, README 절차 실행)은 전부 로컬에서 끝내고, CI에는 로컬로 대체할 수 없는 검증(배포
  자산 기반 빌드, 다른 OS·러너)만 맡긴다. 결함 하나를 고치고 push해 CI 결과로 다음 결함을 찾는 방식은
  쓰지 않는다 — CI 실패 로그에서 모든 job·step의 원인을 한 번에 모으고, 로컬에서 재현·수정·확인한 뒤
  한 번 push한다(cpp tutorial CI 한 회가 25분이다).
- Core를 바꾸고 Framework에서 검증할 때는 local Core library와 binding package가 실제로 갱신됐는지
  먼저 확인한다. 세부 절차는 `scripts/local-package/README.ko.md`를 따른다.
- 완료 보고에는 결과, 변경 파일, 실행한 test와 남은 실패만 적는다. 진행 이력을 반복하지 않는다.
- Runtime 변경의 완료 보고에는 소유 계층·spec 조항·교차언어 대조 결과·변경 분류(A/B/C/D)를 한 줄씩
  함께 적는다. 이 네 줄이 없거나 분류가 C/D인 변경은 감독이 커밋하지 않는다.
- 사용자와의 한국어 기술 설명은
  [`doc/principal/documentation/documentation-principles.ko.md`](./doc/principal/documentation/documentation-principles.ko.md)를 따른다.

### 4.1 간헐 실패 디버깅 (message tracking·file log)

[`framework/doc/framework/common/spec/server/README.ko.md`](./framework/doc/framework/common/spec/server/README.ko.md)의
"디버깅 원칙"과 [`26-message-flow-tracing`](./framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md)·
[`27-flow-correlation`](./framework/doc/framework/common/spec/server/06-observability/04-flow-correlation.ko.md)을 따른다.

- **먼저 이미 있는 message tracking과 file log를 켜서 읽는다.** 임시 로깅부터 추가하고 재현을
  다시 돌리지 않는다 — 재현 사이클 하나를 예외 한 줄 보는 데 낭비하고, 이미 flow에 찍힌 원인을 놓친다.
- message tracking = framework message-flow 트레이싱(`runtime.Flow`/`ZLinkMessageFlowOutcome`).
  프로세스 경계를 넘어 `flow`·`corr`로 메시지를 잇고, 실패·거부·abort를 같은 `flow` 아래
  `message_flow_outcome=error`(errorType/errorMessage)로 남긴다. 통과 케이스와 실패 케이스의
  트레이스를 나란히 놓고 **어느 transition에서 멈췄는지** 찾는다. 카테고리를 노이즈로 필터링하지 않는다.
- 켜는 법: dispatch diagnostics 레벨을 `Normal`(필요 시 `Detailed`)로 올리고 flow listener를
  파일에 배선한다(크로스랭 dotnet TestHost는 `<EventFilePath>.flow`; 시나리오 configurator가
  listener를 안 걸었으면 stream-raw 노드처럼 걸어 켜는 것도 "기존 기능 켜기"다). 보조 trace:
  cpp/.NET `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY`, java/kotlin `ZLINK_JAVA_STREAM_TRACE=1`,
  run dir 보존 `ZLINK_CPP_CROSS_KEEP_RUN_DIR=1`. **첫 재현부터 로그를 보존한다.**
- 임시 로깅은 필요하면 추가할 수 있으나 **조사 후 삭제한다**. 반복·중요 transition이면 임시로
  두지 말고 spec 26 message-flow 단계로 **정식 승격**하되, README §4 "Cost Rule"을 지킨다 —
  트레이스 off일 때 무비용(hot path는 `if (Flow.Enabled(...))` 래핑, rare는 lazy/thunk로
  event·string·lambda를 게이트 뒤에서만 생성). ungated `Console`/string-concat 로깅을 남기지 않는다.

### 4.2 검증 원칙

[`doc/principal/dev/verification-principles.ko.md`](./doc/principal/dev/verification-principles.ko.md)를
따른다. 두 줄로 요약하면:

- **재현을 먼저 한다.** 결함을 자기 손으로 한 번 일으키지 못했으면 고쳤는지 알 수 없다.
  재현 환경이 없으면 만드는 비용과 CI 왕복 비용을 비교한다 — 대개 만드는 쪽이 싸다.
- **검사는 양방향으로 시험한다.** 나쁜 입력을 일부러 만들어 실제로 잡히는지까지 보고 두
  출력을 함께 보고한다. 통과만 확인한 검사는 무엇을 잡는지 아무도 모른다.

거짓 통과를 새로 겪으면 그 문서 §3에 한 줄을 더한다. 이 절은 늘리지 않는다.

## 5. 문서 보호

다음 경로는 사용자가 해당 경로와 변경 범위를 명시적으로 승인한 경우에만 생성·수정·삭제·이동한다.

- `core/doc/internals/`, `core/doc/spec/`
- `bindings/doc/spec/`
- `framework/doc/framework/common/{sample,e2e,spec,internals}/`

오탈자, link, formatting, generated 결과도 수정에 포함된다. 일반적인 “문서 정리”, 구현 완료나
test 실패는 승인으로 보지 않는다. 승인 없이 필요한 변경을 발견하면 `file:line`, 이유와 제안만
보고한다.

문서를 수정할 때는 해당 범위의 `AGENTS.md`를 추가로 따른다.

### 5.1 스펙 개정 절차

- **스펙은 감독자만 고친다.** sub-agent(codex·Claude)에게 스펙 파일의 수정을 맡기지 않는다. sub-agent는
  스펙 gap을 `file:line`과 함께 BLOCKERS로 보고하고, 초안이 필요하면 스펙 밖 산출물(summary·inventory)에
  적는다.
- **개정은 규칙이 줄어드는 방향으로만 한다.** 사실 하나에 소유 문서 하나, 결정 하나에 규칙 하나. 언어별
  예외·"이 경우만" 분기·같은 규칙의 두 번째 서술을 넣지 않는다. 제어(누가 결정하는가)는 한 계층·한 절에
  둔다. 개정 전/후 규칙 수를 결정 기록에 한 줄로 적는다.
- **수정 뒤 codex `sol` 리뷰를 거쳐 확정한다.** 스펙 diff만 대상으로 읽기 전용 리뷰 job을 `gpt-6-sol`로
  돌리고(기준: 사실 정확성, 소유자 하나, 규칙 수, ko/en 동치, 링크·anchor), 반려 항목을 반영한 뒤에만
  스펙을 확정하고 구현 job을 시작한다. 리뷰 없이 스펙과 구현을 같은 커밋에 넣지 않는다.

## 6. 범위별 규칙

- 문서 작성 전반: `doc/AGENTS.md`
- Framework와 public contract parity: `framework/AGENTS.md`
- Framework 문서 위치와 계약 작성: `framework/doc/AGENTS.md`
- .NET Framework의 binding 사용: `framework/languages/dotnet/AGENTS.md`
- Core·binding local package 작업: `scripts/local-package/README.ko.md`
- C binding benchmark: `bindings/c/perf/AGENTS.md`

하위 `AGENTS.md`는 자신의 디렉터리에서만 루트 규칙을 보완한다. 같은 내용을 루트에 다시 복사하지 않는다.
