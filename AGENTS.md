# Agent Guidelines

이 파일은 저장소 전체에 적용하는 최소 공통 규칙이다. 특정 디렉터리의 상세 규칙은 그
디렉터리의 `AGENTS.md`가 소유한다. `CLAUDE.md`는 이 파일을 단일 진입점으로 참조한다.

## 1. 작업 시작과 변경 보호

- 작업 전에 `git branch --show-current`를 확인한다. 사용자가 다른 branch를 지정하지 않으면
  `main`에서만 수정, commit과 push를 수행한다.
- 다른 branch이고 worktree가 clean하면 `main`으로 전환한다. 변경이 있으면 먼저 범위를
  보고하며, 승인 없이 branch 전환, `reset`, `restore`, 강제 checkout 또는 삭제를 하지 않는다.
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

### 2.1 Sub-agent model·추론 레벨

- Sub-agent를 투입할 때는 사용하는 도구(Claude, Codex 등)에서 그 작업 난이도에 맞는 model과
  추론 레벨을 고른다. 지정하지 않으면 세션 설정을 상속하므로, 기계적 작업이면 명시적으로 낮춘다.
- 큰 작업은 단계로 나눠 정찰·수집을 가벼운 agent에 먼저 맡기고, 무거운 model은 판단과 설계가
  필요한 단계에만 투입한다.
- 이미 실행 중인 agent는 model을 바꾸려고 중단·재투입하지 않는다. 재작업 비용이 더 크다.

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
- POSDDD를 명시적으로 요청받으면
  [`doc/principal/dev/posddd.ko.md`](./doc/principal/dev/posddd.ko.md)와
  [`doc/principal/dev/zlink-system-design-principles.ko.md`](./doc/principal/dev/zlink-system-design-principles.ko.md)를 읽고 적용한다.

## 4. 검증과 완료 보고

- 수정 중에는 관련 test만 실행한다. 전체 test, E2E, sample과 benchmark는 사용자 요청 또는
  최종 검증 필요성이 있을 때 한 번 실행한다.
- 첫 실제 실패에서 원인을 분리한다. unrelated failure를 임의로 고치거나 expectation을 낮추지 않는다.
- Core를 바꾸고 Framework에서 검증할 때는 local Core library와 binding package가 실제로 갱신됐는지
  먼저 확인한다. 세부 절차는 `scripts/local-package/README.ko.md`를 따른다.
- 완료 보고에는 결과, 변경 파일, 실행한 test와 남은 실패만 적는다. 진행 이력을 반복하지 않는다.
- 사용자와의 한국어 기술 설명은
  [`doc/principal/documentation/documentation-principles.ko.md`](./doc/principal/documentation/documentation-principles.ko.md)를 따른다.

### 4.1 간헐 실패 디버깅 (message tracking·file log)

[`framework/doc/framework/common/spec/server/README.ko.md`](./framework/doc/framework/common/spec/server/README.ko.md)의
"디버깅 원칙"과 [`26-message-flow-tracing`](./framework/doc/framework/common/spec/server/26-message-flow-tracing.ko.md)·
[`27-flow-correlation`](./framework/doc/framework/common/spec/server/27-flow-correlation.ko.md)을 따른다.

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

## 5. 문서 보호

다음 경로는 사용자가 해당 경로와 변경 범위를 명시적으로 승인한 경우에만 생성·수정·삭제·이동한다.

- `core/doc/internals/`, `core/doc/spec/`
- `bindings/doc/spec/`
- `framework/doc/framework/common/{sample,e2e,spec,internals}/`

오탈자, link, formatting, generated 결과도 수정에 포함된다. 일반적인 “문서 정리”, 구현 완료나
test 실패는 승인으로 보지 않는다. 승인 없이 필요한 변경을 발견하면 `file:line`, 이유와 제안만
보고한다.

문서를 수정할 때는 해당 범위의 `AGENTS.md`를 추가로 따른다.

## 6. 범위별 규칙

- 문서 작성 전반: `doc/AGENTS.md`
- Framework와 public contract parity: `framework/AGENTS.md`
- Framework 문서 위치와 계약 작성: `framework/doc/AGENTS.md`
- .NET Framework의 binding 사용: `framework/languages/dotnet/AGENTS.md`
- Core·binding local package 작업: `scripts/local-package/README.ko.md`
- C binding benchmark: `bindings/c/perf/AGENTS.md`

하위 `AGENTS.md`는 자신의 디렉터리에서만 루트 규칙을 보완한다. 같은 내용을 루트에 다시 복사하지 않는다.
