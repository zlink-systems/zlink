# Agent Guidelines

이 파일은 저장소 전체에 적용하는 최소 공통 규칙이다. 특정 디렉터리의 상세 규칙은 그
디렉터리의 `AGENTS.md`가 소유한다. `CLAUDE.md`는 이 파일을 단일 진입점으로 참조한다.
사람용 절차와 위치(빌드, gate, 성능 판정, 릴리스, 판정 기록)는 [`CONTRIBUTING.ko.md`](./CONTRIBUTING.ko.md)가 소유한다.
내부 운영 절차(sub-agent 운영, 감독자 리뷰, 보드·worktree 위치)는 비공개 저장소 zlink-systems/docs의
`zlink/AGENTS.md`가 소유한다. 세션을 시작할 때 `bash scripts/dev/internal-docs.sh`를 실행하고 출력된 파일을 읽는다.
이 스크립트는 어느 머신·worktree에서든 기본 사본 옆 `zlink-docs`에 저장소를 받거나 최신화하고, Claude sub-agent 정의를
`~/.claude/agents/`로 복사한다. 받을 권한이 없으면 공개 규칙만 따른다.

## 1. 작업 시작과 변경 보호

- 작업 전에 `git branch --show-current`를 확인하고, 사용자가 지정한 branch에서만 수정, commit과
  push를 수행한다. 어느 branch인지 분명하지 않으면 먼저 묻는다.
  작업 단위는 Issue 브랜치(`<area>/<issue번호>-<slug>`)와 worktree이며, main은 PR로 바뀐다.
  문서만 바꾸는 수정과 간단한 작업도 PR로 넣는다 —
  간단한 작업은 별도 worktree를 생략할 수 있다.
- **작업 중에 드러난 결함·누락은 새 Issue를 만들지 않고 그 작업의 Issue·PR 안에서 처리한다.** 여러 언어에 걸친
  결함도 Issue 하나에서 함께 고친다. 새 Issue는 이번 릴리스에서 다루지 않고 넘기는 일에만 연다.
  한 Issue의 작업이 커서 나눠 진행할 때는 그 Issue를 에픽으로 두고 GitHub 하위 이슈(sub-issue)로 나누며,
  진행 상태는 각 하위 이슈가 소유한다.
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
- **[제어 결정과 요청 수락의 소유](./doc/principal/dev/zlink-system-design-principles.ko.md#제어-결정과-요청-수락의-소유)(필수).**
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
  처리하는지 확인한다. **언어마다 동작이 다르면 한 언어만 고치지 않는다** — 스펙이 그 결정을 정하지
  않은 것(D)이므로 먼저 스펙을 정하고([5.1](#51-스펙-개정-절차)) 같은 작업에서 네 언어를 맞추며,
  교차언어 contract test로 네 언어가 같은 결과를 내는지 확인한다.
- **원칙이 단순해지는 근본 수정(필수).** 여기서 "단순"은 변경량이 아니라 **결과 설계의 규칙 수**다. 수정 뒤에 사실 하나에
  소유자 하나, 결정 하나에 규칙 하나, 특수 case·예외 경로·조건 분기가 줄어들어야 한다. 규칙을 줄이기 위한 큰 diff는
  허용하고, 규칙을 하나 더 얹는 작은 diff(새 상태·타이머·인덱스·헬퍼 계층·옵션·"이 경우만" 분기)는 금지한다. 두 대안이 있으면
  설명해야 할 규칙이 적은 쪽을 고르고, 완료 보고에 "수정 전/후 규칙 수"를 한 줄로 적는다. 규칙 수는
  그 결정을 내리는 곳(판정 위치)의 수를 세어 숫자로 적는다. 새 상태·플래그·queue·lock을 더했으면 무엇을
  지웠는지 함께 적는다. 같은 규칙·상태·헬퍼·매핑표를 두 곳에
  두지 않는다(중복 금지): 이미 있는 소유자를 재사용하고, 새로 만들어야 하면 기존 것을 그 자리로 옮겨 하나만 남긴다.
- **진단 먼저, 구현은 승인 뒤.** Framework runtime 수정은 두 단계로 한다. 1단계는 코드 변경 없이
  먼저 **결정 소유 표**를 만든다: 실패에 관여한 결정마다 스펙의 소유 조항, 지금 그 결정을 내리는 곳 전부
  (네 언어 `file:line`), 하나로 모을 소유자. 증상에 가장 가까운 코드에 조건을 더하는 수정은 이 표 없이
  제안하지 않는다. 그다음 원인 `file:line`, 소유 계층과 spec 조항, 교차언어 대조, 변경 분류(A 계약 적응 /
  B 기존 결함 / C 우회 / D spec gap)를 보고한다. 감독이 A 또는 B로 승인한 뒤에만 2단계 구현을 시작한다.
- POSDDD 원칙 문서
  [`doc/principal/dev/posddd.ko.md`](./doc/principal/dev/posddd.ko.md)와
  [`doc/principal/dev/zlink-system-design-principles.ko.md`](./doc/principal/dev/zlink-system-design-principles.ko.md)는
  요청과 무관하게 runtime 변경 시 항상 적용한다.
- **리팩토링은 변경의 일부다(필수).** 별도 리팩토링 이슈로 미루지 않는다. 기준은 성능, POSDDD,
  불필요한 코드(호출되지 않는 코드, 같은 의미의 helper·DTO·adapter 중복, 쓰이지 않는 옵션·분기) 세 가지다.
  - **PR마다:** 건드린 모듈과 그 호출 경로를 세 기준으로 점검하고, 발견은 같은 PR에서 고친다.
    범위를 크게 넘는 발견은 `file:line`과 함께 그 릴리스 마일스톤의 이슈에 적는다.
  - **릴리스 전:** 네 언어 framework·connector 전체를 전수 감사한다 — 한 결정의 판정자가 둘 이상인 곳,
    아래 gate 규칙 위반, Core·binding 소유 결정의 재구현. 발견이 0이 된 뒤 태그한다.
  - **스펙 gap을 만들지 않는다.** 리팩토링은 스펙이 정한 동작을 바꾸지 않는다. 스펙에 없는 동작이
    필요해 보이면 멈추고 보고한다([5.1](#51-스펙-개정-절차)).
  - **Execution gate를 우회하는 동기화를 남기지 않는다.**
    [`02-handler-turn-and-execution-gate.ko.md`](./framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md)가
    기준이다. Framework 내부 구현(runtime·dispatch·connector)에서 다음 세 가지를 찾아, gate 하나가 결정하도록 고친다.
    - gate가 이미 직렬화하는 상태를 lock·별도 queue·대기로 다시 보호하는 코드
    - 같은 실행 권한을 플래그·상태·generation 같은 두 번째 수단으로 판정하는 코드
    - handler turn 안에서 infrastructure 진행을 기다리는 코드

    sample·perf 같은 애플리케이션 코드가 handler 안에서 public 비동기 API(request, Actor 생성·bind 등)를
    기다리는 것은 public 계약이 허용한 사용이며 이 항목의 대상이 아니다.
  - 완료 보고에 점검 결과(발견 수, 고친 항목, 넘긴 항목)를 한 줄로 적는다.

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
- **글의 문체는 [`documentation-principles.ko.md`](./doc/principal/documentation/documentation-principles.ko.md) 원칙 7을 따른다.**
  문서뿐 아니라 이슈·PR·커밋 본문, 코드 주석, 리뷰 코멘트, 완료 보고, 사용자에게 보내는 답변까지 적용 대상이며
  (원칙 7.11), sub-agent도 같다 — 브리프에 이 링크를 넣는다. 비유·구어 표현의 판정과 바꿔 쓴 예는 원칙 7.7의 표가 소유한다.

### 4.1 간헐 실패 디버깅 (message tracking·file log)

[`framework/doc/framework/common/spec/server/README.ko.md`](./framework/doc/framework/common/spec/server/README.ko.md)의
"디버깅 원칙"과 [`26-message-flow-tracing`](./framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md)·
[`27-flow-correlation`](./framework/doc/framework/common/spec/server/06-observability/04-flow-correlation.ko.md)을 따른다.

- **격리 실행에서 한 번이라도 실패하면 결함이다.** 재실행 통과를 판정 근거로 쓰지 않는다. 흩어진
  제어는 대개 타이밍에서만 드러나므로, 실패 하나가 유일한 증거일 수 있다.
- **먼저 이미 있는 message tracking과 file log를 켜서 읽는다.** 임시 로깅부터 추가하고 재현을
  다시 돌리지 않는다 — 재현 사이클 하나를 예외 한 줄 보는 데 낭비하고, 이미 flow에 찍힌 원인을 놓친다.
- message tracking = framework message-flow 트레이싱(`runtime.Flow`/`ZLinkMessageFlowOutcome`).
  프로세스 경계를 넘어 `flow`·`corr`로 메시지를 잇고, 실패·거부·abort를 같은 `flow` 아래
  `message_flow_outcome=error`(errorType/errorMessage)로 남긴다. 통과 케이스와 실패 케이스의
  트레이스를 나란히 놓고 **어느 transition에서 멈췄는지** 찾는다. 카테고리를 노이즈로 필터링하지 않는다.
- 켜는 법: dispatch diagnostics 레벨을 `Normal`(필요 시 `Detailed`)로 올리고 flow listener를
  파일에 배선한다(크로스랭 dotnet TestHost는 `<EventFilePath>.flow`; 시나리오 configurator가
  listener를 안 걸었으면 stream-raw 노드처럼 걸어 켜는 것도 "기존 기능 켜기"다). 보조 trace:
  .NET `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY`, cpp `ZLINK_CPP_MESH_TRACE=1`·`ZLINK_CPP_AUTO_CONNECT_TRACE=1`, java/kotlin `ZLINK_JAVA_STREAM_TRACE=1`,
  run dir 보존 `ZLINK_CPP_CROSS_KEEP_RUN_DIR=1`. **첫 재현부터 로그를 보존한다.**
- 임시 로깅은 필요하면 추가할 수 있으나 **조사 후 삭제한다**. 반복·중요 transition이면 임시로
  두지 말고 spec 26 message-flow 단계로 **정식 승격**하되, README §4 "Cost Rule"을 지킨다 —
  트레이스 off일 때 무비용(hot path는 `if (Flow.Enabled(...))` 래핑, rare는 lazy/thunk로
  event·string·lambda를 게이트 뒤에서만 생성). ungated `Console`/string-concat 로깅을 남기지 않는다.

### 4.2 검증 원칙

원칙은 두 가지다:

- **재현을 먼저 한다.** 결함을 자기 손으로 한 번 일으키지 못했으면 고쳤는지 알 수 없다.
  재현 환경이 없으면 만드는 비용과 CI 왕복 비용을 비교한다 — 대개 만드는 쪽이 싸다.
- **검사는 양방향으로 시험한다.** 나쁜 입력을 일부러 만들어 실제로 잡히는지까지 보고 두
  출력을 함께 보고한다. 통과만 확인한 검사는 무엇을 잡는지 아무도 모른다.

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

## 6. 범위별 규칙

- 문서 작성 전반: `doc/AGENTS.md`
- Framework와 public contract parity: `framework/AGENTS.md`
- Framework 문서 위치와 계약 작성: `framework/doc/AGENTS.md`
- .NET Framework의 binding 사용: `framework/languages/dotnet/AGENTS.md`
- Core·binding local package 작업: `scripts/local-package/README.ko.md`
- C binding benchmark: `bindings/c/perf/AGENTS.md`

하위 `AGENTS.md`는 자신의 디렉터리에서만 루트 규칙을 보완한다. 같은 내용을 루트에 다시 복사하지 않는다.
