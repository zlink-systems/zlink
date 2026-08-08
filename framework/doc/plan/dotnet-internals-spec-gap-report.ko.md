---
title: ".NET Framework server 스펙 구현 Gap 리포트"
---

# .NET Framework server 스펙 구현 Gap 리포트

- **작성일**: 2026-08-08
- **공개 계약 기준**: 현재 worktree의 `framework/doc/framework/common/spec/`와 .NET exact interface
- **내부 구조 기준**: 현재 worktree의 `framework/doc/framework/common/internals/` 01–12
- **메시지 크기 계약 보정**: 2026-08-07 사용자 확인에 따라 RouteMesh ServerServer에는 Framework message-size 상한이 없다. 별도의 기본 64 KiB·startup 설정 계약은 외부 Client가 StreamNode의 Core STREAM socket으로 보내는 C→S complete message에만 적용한다. ClientServer Channel은 기존 negotiated complete-message 계약을 유지한다.
- **구현 기준**: 최초 audit의 기준 commit은 `425b9c2a8272`이고, 최신 판정의 current `main` HEAD는 `cca88f92ac06d910d65bb2a1202ad1b970e44492`이다. 동일 Sol 재검토는 초기 후보 `137f2858bf7fd29f58405893473be8e773725a93`와 current HEAD 위의 worktree 변경을 함께 대조했다. 중간 checkpoint와 검증 증거는 각 상세 항목에 기록한다.
- **판정 방법**: exact public declaration, production 호출 경로, 오류·수명주기·동시성·HWM 의미, service wire codec과 실제 test assertion을 차례로 대조했다. Type이나 method가 존재하는지만으로 완료 판정하지 않았다. 2차 검토는 `gpt-5.6-sol` high 독립 reviewer가 기존 판정을 반증하고 누락을 찾은 뒤, 지적된 경로를 현재 source에서 다시 확인했다.
- **현재 상태 우선 규칙**: 중간 checkpoint의 당시 count·hash·process path는 historical evidence로 보존한다. 현재 판정은 1장 요약 표, 5장 검증 표와 문서 마지막의 최신 동일 Sol 재검토 항목을 우선한다.
- **2026-08-08 session 교체 재검토**: `DOTNET-SESS-REPLACE-001`의 구현·package·세 process 증거를 추가로 확인했다. 정식 spec·schema·golden은 이번 재검토에서 수정하지 않았고 기존 dirty 변경을 보존했으며, 이 계획서만 현재 구현 상태에 맞춰 갱신한다.

최초 audit에서 확인한 `.NET Framework server` 구현 gap은 각 항목별 public exact interface, runtime·구조·비용
의미와 owner-layer regression 기준으로 조사했고, 완료·부분·blocked 상태를 아래에 분리해 기록했다. 각 상세
항목에 package와 관련 process 증거를 기록했지만, 전체 종결은 별도의 canonical integration·release·process gate를
요구한다.
Timer option은 공통 spec의 normalization 규칙을 재확인해 language exact interface와 canonical wire schema가
충돌하지 않는 것으로 판정을 바로잡고 세 policy의 canonical round-trip test를 추가했다. 이후 추가된
session 교체 계약은 happy-path source·package·process evidence를 확인했다. 2026-08-08 재검토에서 찾은
callback failure 분류 오류와 owner-layer regression 누락은 production source와 deterministic regression으로
보완했다. 그러나 canonical protocol source가 `main`에 통합되지 않았고 보호된 구현 차이 문서 두 곳이 현재
runtime과 다르며 Windows PowerShell evidence도 남아 있다. 따라서 전체 gap 종결 조건은 아직 충족하지 않는다.

Public API와 사용자에게 보이는 동작은 정식 spec과 exact interface만 계약 근거로 사용한다. 구조·POSDDD
gap은 그 계약을 구현하는 internals의 상태 표현, component 책임이나 불변 조건과 다르다는 판정이며,
internals 자체가 새 공개 계약을 만들지는 않는다. 현재 source만으로 사용자-visible 장애나 성능 저하
수치까지 측정됐다는 뜻도 아니며, 그런 주장은 별도 benchmark/process evidence가 있어야 한다.

## 병렬 구현 세션 주의 사항

이 보고서는 다른 언어의 gap 작업과 동시에 진행할 수 있지만, 모든 작업은 현재 `main` checkout에서
수행한다. 별도 `git worktree`나 작업용 branch를 만들지 않으며, 작업 시작 시점의 `main` commit SHA를
작업 기록에 남긴다.

- 이 세션은 해당 언어의 production source·test·package 자료와 이 gap 문서만 수정한다. 다른 언어
  디렉터리, 다른 언어 gap 문서와 공통 spec·internals는 수정하지 않는다.
- `framework/runtime/protocol/`의 schema·generated 파일, cross-language fixture, 공통 검증 script처럼
  여러 언어가 함께 소비하는 파일은 통합 담당자 한 명만 수정한다. 변경이 필요하면 이 문서에 요구사항과
  예상 wire/API 영향을 기록하고 공용 선행 commit을 요청한다.
- 다른 세션의 변경을 원복하거나 포맷하지 않는다. Stage와 commit은 명시적인 경로 목록으로 제한하고
  `git add -A`를 사용하지 않는다.
- Gap 종결은 source 수정만으로 판단하지 않는다. Owner-layer regression, public API/exact snapshot,
  package 또는 clean-consumer, 관련 process E2E 증거를 각각 기록하고 통과한 항목만 종결한다.
- 언어별 작업이 `main`에 반영된 뒤 통합 담당자가 cross-language contract, service-wire fixture, 전체 문서
  검사와 process E2E를 다시 실행한다. 개별 성공을 전체 종결로 승격하지 않는다.

### Blocker 처리 규칙

스펙 변경 승인, 외부 process 환경, 다른 담당자의 통합처럼 현재 작업 범위를 벗어난 조건이 발견되면
해당 항목을 즉시 `BLOCKED`로 기록하고 다음 정보를 함께 남긴다.

- blocker를 발생시킨 계약·소스·검증 결과의 정확한 `file:line`과 기준 commit
- 현재 통과한 범위와 통과하지 못한 범위, 그리고 blocker가 없으면 실행할 gate
- 필요한 승인·통합 담당자·외부 환경과, 그 조건이 충족되기 전에는 완료로 판정할 수 없는 이유
- blocker와 독립적으로 진행할 수 있는 production/test/package/process 작업과 그 결과

Blocker를 없애기 위해 정식 spec, canonical protocol source 또는 보호 문서를 임의로 수정하지 않는다.
승인이나 외부 변경을 기다리는 동안에는 독립적인 구현·회귀·package·process 검증을 계속하고, 각 결과를
해당 항목과 검증 표에 기록한다. 이후 blocker 조건이 바뀌면 같은 기준 commit 또는 새 candidate를 명시해
관련 gate를 다시 실행한다.

### 구현 중 리팩터링·checkpoint 규칙

Gap 하나 또는 서로 강하게 연결된 작은 작업 묶음의 동작과 회귀 test가 통과하면 다음 Gap으로 넘어가기
전에 리팩터링 checkpoint를 둔다. 마지막에 한꺼번에 정리하지 않는다.

- Production code는 POSD 관점에서 deep module과 information hiding을 강화하고, 의미 없이 인자를 전달하는
  pass-through 계층, 호출 순서에 의존하는 temporal decomposition과 중복 helper를 제거한다. DDD 관점에서는
  lifecycle·ownership·state transition·terminal error invariant를 해당 domain owner가 책임하게 정리한다.
- 같은 checkpoint에서 unit test도 POSD/DDD 관점으로 리팩터링한다. 반복 setup은 의도를 드러내는 fixture나
  builder 안에 숨기고, test 이름과 helper는 domain 용어와 observable behavior를 표현하게 한다. Production
  내부 구조를 그대로 복제하거나 실행 순서와 private 구현에 결합된 test는 제거하거나 계약 중심으로 바꾼다.
- 리팩터링 뒤 dead code, 사용하지 않는 wrapper·alias·fixture·dependency를 제거하고, hot path의 불필요한
  allocation·copy와 lock·queue contention도 함께 점검한다. 동작 변경이 있으면 owner-layer regression을
  먼저 추가하고 관련 unit test를 다시 실행한다.
- 관련 test가 통과한 의미 있는 checkpoint마다 해당 언어 경로와 이 문서만 path-limited staging하여
  commit하고 `main`에 push한다. Commit에는 닫은 Gap ID와 실행한 test를 남기고, 검증되지 않은 변경이나
  다른 언어의 변경을 섞지 않는다. Push한 commit SHA와 gate 결과를 이 문서의 해당 항목에 기록한 뒤
  다음 작업으로 진행한다.

### 최종 종료 전 Codex Sol 검토 관문

최종 종료 판정 전에는 해당 언어의 모든 Gap·PARTIAL·public contract 항목을 대상으로 `Codex Sol`
review를 수행한다. 요약이나 focused test 통과 여부가 아니라 항목별 exact interface, 정식 spec,
production runtime, owner-layer regression, package/clean-consumer와 process evidence를 서로 대조해
다음 사항을 확인한다.

- 항목이 누락되지 않았는지, 완료로 표시한 구현이 실제 계약과 다른 부분이 없는지 확인한다.
- 누락·오판·부분 구현을 발견하면 해당 항목을 GAP 또는 BLOCKED로 되돌리고 owner-layer 수정과 회귀
  증거를 추가한 뒤 같은 Codex Sol review를 반복한다.
- review 대상, 사용한 Codex Sol 모델/effort, 기준 commit 또는 candidate manifest, 발견 사항, 수정
  commit, 재실행한 gate와 판정을 이 문서에 `file:line` 근거와 함께 기록한다. 단일 test, 문서 존재,
  source compile 또는 과거 결과만으로 항목을 clean 처리하지 않는다.

모든 계약·구현 항목이 위 review에서 누락 없이 구현되었다는 판정을 받은 뒤에만 2차 구조 review를
시작한다. 2차 review도 동일한 `Codex Sol`을 사용하며, 대상은 해당 언어의 Framework runtime
production source와 unit test다. 실행 순서는 먼저 production runtime 리팩터링과 회귀 검증을
완료한 뒤 unit test 리팩터링을 진행하는 것으로 고정한다. 다음 네 범주를 각각 검토하고 결과를 기록한다.

1. **성능 비용** — 불필요한 allocation·copy, payload 변환 왕복, lock/mutex/channel/atomic/queue
   contention, hot path의 중복 작업을 확인한다.
2. **불필요한 코드** — dead code와 도달하지 않는 branch, 사용하지 않는 wrapper·alias·helper·fixture·
   파일·dependency를 확인한다.
3. **POSD/DDD 구조** — deep module·information hiding, pass-through와 temporal decomposition,
   caller complexity, 중복 책임을 POSD 관점에서 확인하고 lifecycle·ownership·state transition·
   commit/deadline·terminal failure invariant의 domain owner가 명확한지 DDD 관점에서 확인한다.
4. **unit test 구조** — runtime 리팩터링으로 보존해야 할 observable behavior와 domain invariant를
   기준으로 test를 다시 읽는다. POSD/DDD 관점에서 동일한 의도·계약·fixture를 반복하는 중복 unit
   test는 하나의 명확한 test 또는 공통 parameterized/fixture test로 통합하고, 의미 없는 복제 test,
   private 구현·호출 순서에만 결합된 test는 회귀 증거를 보존한 뒤 제거한다. 통합·삭제 후에는 해당
   owner-layer regression과 aggregate gate를 다시 실행한다.

2차 review에서 Medium 이상 finding이 하나라도 남으면 clean으로 판정하지 않는다. 해당 runtime/test를
수정하고 필요한 owner-layer regression 및 관련 gate를 다시 실행한 뒤 같은 Codex Sol review를
반복한다. Low finding도 처리하거나 명시적으로 잔여 위험으로 승인 기록해야 한다. 두 단계의 review
결과가 모두 `CLEAN`, Medium 이상 `0`, 미실행 필수 gate `0`으로 기록된 경우에만 이 문서의 전체 작업을
완료로 판정한다.

## 1. 판정 요약

| ID | 심각도 | 영역 | 판정 |
|---|---|---|---|
| DOTNET-API-001 | 상 | STREAM session send | 완료 — per-call admission timeout을 public surface와 기존 submit admission owner에 연결했다 |
| DOTNET-API-002 | 상 | Location 운영 query | 완료 — authority Store를 조회해 exact/list object location을 공개 계약으로 투영한다 |
| DOTNET-API-003 | 상 | StreamNode message-size API | 완료 — fluent `MaxMessageSize(bytes)`와 전용 socket config surface를 제공한다 |
| DOTNET-ROLE-001 | 상 | ClientServer role error | 완료 — Server role만 등록된 Channel send/request가 `NotConfigured`로 끝난다 |
| DOTNET-WIRE-001 | 상 | `framework-json-v1` | 완료 — application payload 전용 strict codec이 canonical golden profile을 강제한다 |
| DOTNET-SIZE-001 | 상 | RouteMesh SS message size | 완료 — public·admission·native receive·sender 상한을 제거하고 HWM과 wire guard만 유지한다 |
| DOTNET-WIRE-002 | 상 | ClientServer message bound | 완료 — negotiated complete-message 상한을 send/request/inbound/reply에 같은 계산으로 적용한다 |
| DOTNET-COMP-001 | 상 | completion overflow | 완료 — retention 포화가 pending과 이후 caller에게 `CapacityExceeded` terminal로 전달된다 |
| DOTNET-EXEC-001 | 상 | STREAM session queue | 완료 — session과 생성 ingress queue가 retained payload byte와 고정 비용을 함께 예약한다 |
| DOTNET-EXEC-002 | 중 | serial execution engine | 완료 — Spot/session/Actor 전달이 공통 serial engine의 lane policy로 수렴했다 |
| DOTNET-EXEC-003 | 상 | Entry Actor ingress HWM | 완료 — process-wide ingress와 Actor별 공통 serial lane이 count·retained byte를 제한한다 |
| DOTNET-LIFE-001 | 중 | Spot type model | 완료 — 공통 resource base 위에 User와 Instance activation type과 context surface를 분리했다 |
| DOTNET-LIFE-002 | 중 | Ready Instance owner loss | 완료 — 실제 owner crash·restart 뒤 `Unavailable`과 queue replay 부재를 process에서 검증했다 |
| DOTNET-COMP-002 | 중 | completion ordering | 완료 — sender가 응답 상관 값을 먼저 할당하고 waiter 등록 뒤 submit한다 |
| DOTNET-LAYER-001 | 중 | binding 경계·POSDDD | 완료 — 일반 socket은 binding public API를 직접 쓰고 의미 변환 adapter만 유지한다 |
| DOTNET-LAYER-002 | 상 | runtime/package ownership | 완료 — lifecycle state machine과 resource close 순서를 Core package가 소유한다 |
| DOTNET-LAYER-003 | 중 | identifier type | 완료 — production owner registry와 core lifecycle parameter를 typed identifier로 고정했고 문자열은 허용된 boundary에만 남겼다. release matrix와 전체 process gate는 별도 항목으로 추적한다 |
| DOTNET-LAYER-004 | 중 | STREAM protocol ownership | 완료 — connector protocol 구현을 client와 Framework server가 함께 사용한다 |
| DOTNET-OWN-001 | 중 | payload ownership/copy | 완료 — public defensive copy는 유지하고 runtime-owned payload는 내부 ownership 이전 경로를 사용한다 |
| SPEC-TIMER-001 | 중 | timer relocation contract | 완료 — non-catch-up의 무시되는 값은 canonical 기본값으로 normalize하고 bounded 값은 보존한다 |
| DOTNET-SESS-REPLACE-001 | 상 | 이전 session replacement callback | **BLOCKED** — runtime과 owner regression은 반영했지만 canonical protocol integration이 formal protocol 변경·승인을 요구한다 |
| DOTNET-SESS-REPLACE-002 | 상 | replacement callback failure | 완료 — application `OperationCanceledException`과 실제 callback deadline을 분리하고 fixed 100 ms grace를 검증한다 |
| DOTNET-SESS-REPLACE-TEST-001 | 상 | replacement lifecycle regression | 완료 — closing·send 허용·fixed timer·deadline·stale/duplicate·direct exact fence lookup·multi-Actor cleanup·admission retry를 owner layer에서 검증한다 |
| DOTNET-SESS-REPLACE-TEST-002 | 중 | replacement transport fence regression | 완료 — 실제 두 Framework runtime의 MeshNode transport가 source/lifecycle admission과 exact owner ID·lease·session RID·binding generation rejection을 runtime handler까지 검증한다 |
| DOTNET-CONTRACT-INTEGRATION-001 | 상 | command 51 canonical source | **BLOCKED** — `main` schema는 51을 reserved로 두며 golden·generator·validator 통합에는 formal protocol 변경·승인이 필요하다 |
| DOTNET-DOC-001 | 중 | .NET exact interface 상태 | **BLOCKED** — 보호 spec의 구현 차이 표가 완료된 runtime을 여전히 미구현으로 기록하며 보호 문서 수정 승인이 필요하다 |
| DOTNET-DOC-002 | 중 | 공통 implementation gap 상태 | **BLOCKED** — 보호된 `90-implementation-gap`의 .NET 행도 command 51·callback이 없다고 기록하며 보호 문서 수정 승인이 필요하다 |
| DOTNET-HTTP-SERIALIZATION-001 | 상 | public ActorRef HTTP JSON | 완료 — `ActorRef`·`SpotRef`가 기본 `System.Text.Json`에서 exact object-reference JSON을 encode/decode하고, invalid ID·generation·mesh·RID와 default encode를 거부한다 |
| DOTNET-WINDOWS-EVIDENCE-001 | 중 | Windows PowerShell runner | **미실행** — regression matrix의 5개 gap이 Windows 환경의 PowerShell runner를 남은 evidence로 명시한다 |
| DOTNET-E2E-INSTANCE-001 | 상 | Config 14 Instance Spot | **미완료** — 36개 중 다수가 미구현이며 IS-E2E-06은 PARTIAL이고 default/all runner가 exit 2다 |
| DOTNET-E2E-PROCESS-001 | 상 | 전체 process Gap·PARTIAL | **미완료** — Instance Spot 외 7개 suite에도 미구현·부분 구현·actual-process 미검증 항목이 남아 있다 |
| DOTNET-SESS-REPLACE-FRESHNESS-001 | 중 | replacement current-candidate process evidence | 완료 — current source ST-E2에서 세 process role evidence와 callback guidance-before-close를 확인했다 |
| DOTNET-SUBMITTER-DISPOSE-RACE-001 | 중 | send-ready·Dispose lifecycle race | 완료 — production terminal conversion, dispose 뒤 callback assertion과 net8/net10 aggregate를 통과했다 |
| DOTNET-RELEASE-MATRIX-001 | 상 | release runtime matrix | **부분 실행** — Linux x64의 net8/net10 Release unit aggregate는 각각 1,622/1,622로 통과했지만 양쪽 TFM의 single/multi-process와 6개 RID 전체 full CI evidence가 없다 |
| DOTNET-UNIT-AGGREGATE-001 | 중 | 전체 unit aggregate gate | 완료 — current typed owner-key candidate에서 net8/net10 Release aggregate가 각각 1,622/1,622로 통과했다 |
| DOTNET-REGRESSION-DOC-001 | 중 | regression matrix 정확성 | 완료 — RouteMesh directional HWM/timeout 기준과 Config 12의 현재 26개 all runner 동작을 반영했다 |
| DOTNET-POSDDD-REVIEW-001 | 중 | runtime/test 구조 review | **대기** — 1차 계약 review가 CLEAN이 아니므로 규정된 2차 POSD/DDD review를 시작하지 않았다 |

표의 ID 행은 추적 단위이고, 동일 원인의 상위·하위 ID를 독립 finding으로 중복 집계하지 않는다.
`DOTNET-SESS-REPLACE-001`과 `DOTNET-CONTRACT-INTEGRATION-001`은 command 51 canonical integration이라는 같은
High blocker를 공유한다. 따라서 최신 Sol의 독립 집계는 High 4건(command 51, Config 14, 전체 process suite,
release matrix)·Medium 3건(보호 exact interface, 보호 implementation-gap, Windows evidence)이다.

## 2. 상세 발견 사항

### DOTNET-API-001 — session send의 per-call admission timeout 누락

**판정: 완료**

Exact interface는 `IZLinkSessionSendCall.Timeout(TimeSpan)`을 요구하고, 이 값이 STREAM socket `SendTimeout`을 늘리지 않고 더 짧게만 적용되며 만료 시 `DeadlineExceeded`로 정확히 한 번 끝나야 한다고 정한다(`interfaces/07-stream-session.ko.md:73-79,143-147`). 최초 audit 기준 commit `425b9c2a8272`의 interface에는 `Compress()`와 `Async(...)`만 있고 `Timeout(...)`이 없었다. 따라서 당시 application은 계약에 적힌 per-call timeout을 표현할 수 없고, runtime에도 양수 millisecond 범위 검증, socket timeout과의 `min`, 만료 뒤 재admission 차단 경로가 없었다. 현재 source의 method는 `Contracts/Streams/IZLinkSession.cs:97-111`에 있다.

완료 조건은 public method와 call implementation을 함께 추가하고 다음을 검증하는 contract/runtime test다.

- 생략 시 socket `SendTimeout` 사용
- 지정 시 두 timeout 중 짧은 값 사용
- 0, 음수와 `Int32.MaxValue` millisecond 초과 거부
- timeout·cancellation·send-ready 경쟁에서 terminal-once
- 만료 뒤 늦은 send-ready가 제출을 시작하지 않음

`IZLinkSessionSendCall.Timeout(...)`을 추가하고, 양수 millisecond 범위로 올림한 값을 기존
`ZLinkAsyncSubmitter` admission deadline에 전달한다. Submitter는 STREAM socket timeout과 per-call timeout
중 짧은 deadline 하나만 만들며, timeout이나 cancellation이 terminal을 먼저 확정하면 이후 send-ready가
같은 operation을 다시 제출하지 않는다.

검증은 .NET 8에서 `ZLinkAsyncSubmitterTests` 32건과 `StreamContracts` 4건이 통과했다. 별도의 source
package를 만든 뒤 실행한 `verify_packaged_contract.sh --generate-snapshot`도 packaged contract와 standalone
HTTP clean consumer를 모두 통과했다. 고정 public API snapshot과 package XML hash를 같은 source 결과로
갱신했다. 이 checkpoint는 `b2ecdc54c3`으로 `main`에 push했다.

### DOTNET-API-002 — object location 운영 query 누락

**판정: 완료**

Exact interface는 object kind/state/entry/filter와 `FindActorLocationAsync`, `FindSpotLocationAsync`, `ListObjectLocationsAsync`를 요구한다(`interfaces/08-location-maintenance.ko.md:106-131,150-178`). 실제 `IZLinkLocationRuntimeQuery`는 status, topology, service summary 세 operation만 제공한다(`Contracts/Locations/RuntimeQuery.cs:8-21`). 구현 service도 MeshNode descriptor만 조회하며 object row를 조회하거나 `Creating`·`Ready`·`Unavailable`로 투영하는 경로가 없다(`Runtime/Locations/ZLinkLocationRuntimeQueryService.cs:63-176`).

이 gap은 정적 추정이 아니라 현재 contract test가 직접 검출했다. `DotNetExactInterfaceDeclarations_Match_Source_And_Package_Exports`와 `DotNetExactInterfaceTypes_Have_A_Single_DocumentOwner`가 `ZLinkLocationObjectEntry` export 부재로 실패했다. Exact lookup의 Missing=`null`, Spot kind 통합, 4 MiB page, Store 실패=`Unavailable`과 partial page 금지까지 production test로 확인해야 완료할 수 있다.

`IZLinkLocationRuntimeQuery`에 exact Actor·Spot 조회와 kind별 list query를 추가했다. 새 내부
`ZLinkLocationObjectQuery`가 authority key 해석, allocation state와 owner lease를 이용한
`Creating`·`Ready`·`Unavailable` projection, opaque continuation과 Store 오류 변환을 한 곳에서 소유한다.
Store key·version과 owner lease generation은 public entry에 노출하지 않는다.

`.NET 8`의 `LocationRuntimeQueryTests` 16건에서 Missing과 세 상태, User Spot·Instance Spot 구분,
filter·continuation, Store read 실패의 typed `Unavailable`, partial page 미반환과 4 MiB 초과 page 거절을
검증했다. `LocationContracts`와 exact type owner 검증 8건도 통과했다. 별도 source package를 생성한
`verify_packaged_contract.sh --generate-snapshot`은 packaged contract와 standalone HTTP clean consumer를
통과했고, 고정 public API snapshot은 생성 결과와 일치하며 hash는
`917792ee8f6a4f645f03b1b683041b819d7501bd0ec54807c728c4fd8ab164e1`이다. 전체 exact declaration
검증에는 이 항목과 무관한 `DOTNET-SIZE-001`의 기존 RouteMesh `MaxMessageSize` property 한 건이 남아 있으며,
해당 항목에서 제거한다.
이 checkpoint는 `9e7628e61e`으로 `main`에 push했다.

### DOTNET-API-003 — StreamNode message-size fluent API 불일치

**판정: 완료**

Exact interface는 StreamNode builder가 `MaxMessageSize(long bytes)`를 직접 제공하고 builder를 반환해 `.Bind(...).MaxMessageSize(64 * 1024).AddSession<...>()` 형태로 연결하도록 정한다(`interfaces/03-configuration-topology.ko.md:247-286`). `ConfigureSocket()`은 HWM·buffer·timeout용 `IZLinkStreamSocketConfig`를 반환하며 message-size property를 노출하지 않는다.

실제 public builder는 `ConfigureSocket()`만 제공하고 `IZLinkSocketConfig`를 반환한다(`Contracts/Configuration/Builders.cs`, `Runtime/Configuration/Builders/ZLinkStreamNodeBuilder.cs:41`). Application은 반환 객체의 mutable `MaxMessageSize` property에 대입해야 하므로 exact fluent call을 컴파일할 수 있고 없는지 여부가 반대로 되어 있다. 기본 64 KiB와 C→S 전용 runtime 적용 자체는 맞지만 public 호출 표면은 스펙과 다르다.

완료 조건은 `IZLinkStreamNodeBuilder.MaxMessageSize(long bytes)`를 추가해 같은 builder를 반환하고, StreamNode의 `ConfigureSocket()` 반환형에서 message-size property를 제거하는 것이다. 64 KiB 기본값, 양수·0·음수 검증과 fluent chaining을 public contract test와 clean consumer에서 확인해야 한다.

`IZLinkStreamNodeBuilder.MaxMessageSize(long bytes)`를 추가하고 `ConfigureSocket()`은 message-size property가
없는 `IZLinkStreamSocketConfig`를 반환하도록 분리했다. 기존 내부 config는 값을 한 곳에서 소유하므로 inbound
runtime 경로와 startup validator에는 별도 복제 상태를 만들지 않았다.

`.NET 8`의 `TopologyExactSurfaceTests` 4건, 관련 inbound validation 1건과 `BuilderContracts` 5건이
통과했다. Source package를 사용한 packaged contract와 standalone HTTP clean consumer도 통과했으며, 생성한
public API snapshot hash는 `68b461cb8de2ff286232a3d17ecc1e30fbd5c540b392dbbc2ba2c5e8b2da680f`다.
이 checkpoint는 `1e30131ff3`으로 `main`에 push했다.

### DOTNET-ROLE-001 — Server-only ClientServer Channel의 오류 kind가 틀림

**판정: 완료**

공통 ClientServer 계약은 같은 `ChannelName`의 Server role이 존재하더라도 local Client role이 없으면 send/request를 시작하지 않고 `NotConfigured`로 끝내며, `NotFound`는 ChannelName이나 선택할 target 자체가 없는 경우에만 사용하도록 정한다(`common/spec/09-client-server-channel.ko.md:57-66`).

실제 send는 등록된 channel의 `HasClientServerClient`가 true일 때만 ClientServer 경로에 들어가고, false이면 RouteMesh lookup으로 fallback한다(`Runtime/Host/ZLinkFrameworkRuntimeChannels.cs:21-63`). Request도 같은 분기다(`:65-110`). Server role만 등록된 같은 이름의 Channel은 존재하지만 Client role 조건을 통과하지 못하며, RouteMesh도 없으면 `ResolveRouteMeshNodeForChannel`이 `NotFound`를 던진다(`Runtime/Host/ZLinkFrameworkRuntimeSpots.cs:271-311`). Server-only send/request의 exact error kind를 주장하는 test도 없다.

완료 조건은 Channel 등록은 존재하지만 Client role이 없음을 topology fallback 전에 구분해 두 call 모두 `ZLinkFrameworkErrorKind.NotConfigured`로 끝내는 것이다. Server-only, 이름 자체 없음, Client role은 있으나 ready target 없음을 각각 분리한 회귀 test가 필요하다.

Channel call path가 ClientServer registration을 먼저 분류하고 local Client role이 없으면 RouteMesh lookup을
시작하지 않도록 바꿨다. 같은 분류를 send와 request가 공유하며, 실패 전에 Framework가 소유한 message parts를
반납한다.

Owner-layer regression은 Server-only send/request=`NotConfigured`, 이름 없음=`NotFound`, Client role은 있지만
ready target 없음=`DeadlineExceeded`를 각각 분리해 3건 모두 통과했다. 실제 process scenario
`CH-E2E-05`도 `logs/20260807-172841-1093480/`에서 통과했으며 Server-only handler evidence가 생성되지
않고 정상 Client role request만 Server handler에 한 번 도달했다.
이 checkpoint는 `c873a99aa0`으로 `main`에 push했다.

### DOTNET-WIRE-001 — `framework-json-v1` strict profile 미구현

**판정: 완료**

공개 message model은 property와 enum name의 대소문자를 구분하고, duplicate·누락 required property를 거부하며, 64-bit integer는 범위를 검사한 정규 decimal **문자열**로 처리하도록 요구한다(`common/spec/04-message-model.ko.md:95-118`). Internals §6도 별도 규칙을 만들지 않고 이 public profile을 runtime validation의 정본으로 위임한다(`common/internals/12-service-wire-protocol.ko.md:272-279`). Golden fixture도 `property-case`, `duplicate-property`, numeric signed64를 invalid로 고정한다(`framework/runtime/protocol/golden/framework-json-v1.json`).

실제 공통 JSON option은 `new JsonSerializerOptions(JsonSerializerDefaults.Web)` 하나뿐이다(`Runtime/Messaging/ZLinkJsonSerializerOptions.cs:6-15`). 이 preset은 스펙 전용 validator가 아니며 property 대소문자, duplicate 탐지, required-field completeness와 64-bit 문자열 정규형을 profile대로 강제하는 단계가 없다. Route/Spot/STREAM typed dispatch는 이 option을 그대로 사용한다(`Runtime/Messaging/ZLinkEnvelopeCodec.cs:368-371`, `Runtime/Streams/ZLinkStreamPacketPayloadCodec.cs:53-54`). Schema self-test 성공은 schema와 golden asset이 유효하다는 뜻일 뿐 이 consumer 동작을 검증하지 않는다.

완료하려면 allocation을 크게 만들기 전에 golden fixture 전체를 검증하는 .NET profile reader를 두고, 네 언어 공통 fixture를 .NET runtime decode 경로에 직접 통과시켜야 한다. 일반 `System.Text.Json` round-trip test는 대체 증거가 아니다.

`ZLinkFrameworkJsonPayloadCodec`을 application typed payload의 단일 JSON 경계로 추가했다. 이 codec은
deserialize 전에 BOM과 중복 property를 거부하고, case-sensitive property, required·nullable 선언,
64-bit 정수의 정규 decimal string, exact enum name, padded base64와 유한 number 규칙을 적용한다. UUID처럼
언어 runtime이 암묵적으로 변환하는 type은 거부하며, 내부 relocation DTO가 UUID를 사용하는 한 곳은
property-level canonical lowercase `D` string converter로 계약을 명시했다. Envelope header와 runtime control
payload는 기존 protocol JSON helper로 분리해 application profile 변경이 내부 wire 표현을 바꾸지 않는다.

공통 `framework-json-v1.json`의 valid·invalid vector 전체와 nested duplicate, BOM, non-nullable null,
비정규 64-bit 문자열, 암묵적 UUID 거절을 `FrameworkJsonProfileTests` 10건에서 실제 envelope decode 경로에
통과시켰다. Envelope·custom codec·Actor·STREAM 관련 78건과 처음 실패한 내부 relay·relocation 회귀 7건이
통과했고, 최종 strict number 설정을 포함한 전체 .NET unit suite도 1,572건 모두 통과했다.
`verify_packaged_contract.sh`의 packaged contract와
standalone HTTP clean consumer가 통과했으며 public API snapshot hash는
`917792ee8f6a4f645f03b1b683041b819d7501bd0ec54807c728c4fd8ab164e1`이다. 실제 process `RegistrationCodec`
`RC-B6`도 `logs/20260807-175928-2425161/`에서 int64·bytes·nullable typed JSON round trip과 handler evidence를
확인하고 통과했다.
이 checkpoint는 `14b720a2d1`으로 `main`에 push했다.

### DOTNET-SIZE-001 — RouteMesh ServerServer에 없어야 할 16 MiB 상한이 있음

**판정: 완료**

확인된 계약은 RouteMesh ServerServer transport에 Framework-level `MaxMessageSize`를 두지 않는 것이다. 공통 channel spec도 이 결정을 그대로 적고 있다(`common/spec/07-channel-topology.ko.md:609-634`). Transport·service-wire 표현 한계와 process memory/HWM은 남지만 별도 complete-message 크기 설정이나 그 이유의 거절은 없어야 한다.

실제 .NET은 `IZLinkMeshNodeSocketConfig.MaxMessageSize`를 public surface로 노출한다(`Contracts/Configuration/MeshNodeBuilders.cs:23-41`). RouteMesh router registration은 일반 `ZLinkSocketConfig`를 사용해 기본 16 MiB를 받고(`Runtime/Configuration/ZLinkFrameworkRegistration.cs:497-513`, `Runtime/Configuration/ZLinkSocketConfigs.cs:6-21`), initializer가 이를 managed MeshNode에 적용한다(`Runtime/Spots/ZLinkSpotNodeInitializer.cs:28-45`). Managed node는 native router receive option에 이 값을 설정할 뿐 아니라(`Runtime/Service/ZLinkManagedMeshNode.cs:248-258`) peer와 작은 값을 골라 모든 send 전에 complete-message 합계를 검사한다(`:8048-8079,8132-8144`). Unit test도 RouteMesh 기본값을 16 MiB로 고정한다(`UnitTests/Configuration/Registration/InboundDispatchOptionsTests.cs:197-217`).

완료 조건은 RouteMesh public `MaxMessageSize` 설정과 admission field, native receive cap 및 sender-side complete-message check를 제거하는 것이다. HWM/mailbox byte budget과 protocol 표현 한계는 별도 자원·wire guard로 유지한다. 회귀 검증은 임의의 새 payload 상한을 암시하지 않도록 public API snapshot, admission wire fixture와 일반 payload 무결성 E2E로 구성한다.

구현과 검증을 완료했다. `IZLinkMeshNodeSocketConfig`에서 `MaxMessageSize`를 제거했고 RouteMesh
admission descriptor에서도 해당 field를 제거해 공통 schema의 field 순서와 맞췄다. Managed RouteMesh
socket은 native inbound cap을 `-1`로 명시해 binding이나 Core 기본값에 의존하지 않으며, sender와 receiver의
complete-message 비교는 제거했다. HWM, mailbox byte budget, control frame 개수와 .NET byte array 표현 한계는
서로 다른 자원·wire guard로 유지했다.

RouteMesh admission round trip은 lifecycle generation이 security identity 바로 뒤에서 시작하는 정확한 byte
위치와 재인코딩 byte 일치를 검증한다. 1 byte와 기존 기본값을 넘는 17 MiB payload는 실제 managed node
request/reply production socket 경로에서 hash가 아니라 전체 byte 일치로 통과했다. 관련 focused test 23건과
전체 .NET unit suite 1,573건이 통과했다. Packaged contract와 standalone HTTP clean consumer가 통과했으며
public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`이다. 실제 process
`ToActorMessaging` `TA-A1`도 `logs/20260807-183121-3907372/`에서 RouteMesh request/reply와 owner handler
evidence를 확인하고 통과했다.

별도 `LocationMessaging` `RM-C8`은 초기 실행(`logs/20260807-183255-3962502/`와
`logs/20260808-051702-1428832/`)에서 첫 Channel request가 provider handler에 도달하지 않고 timeout됐다.
원인은 production message-size 정책이 아니라 provider E2E host가 byte 단위 native
`ReceiveHighWaterMark=4`를 설정해 complete message보다 작은 transport budget을 만든 것이었다. E2E host의
수신 budget을 4 MiB로 조정하고 Framework application HWM은 그대로 유지한 뒤, `LocationMessaging` 전체
15개 scenario가 `logs/20260808-055401-2923930/`에서 통과했다. 이 설정 보정은 formal spec이나 공통 wire
schema를 변경하지 않는다.
이 checkpoint는 `ce9b881ae0`으로 `main`에 push했다.

### DOTNET-WIRE-002 — ClientServer negotiated complete-message 상한 미강제

**판정: 완료**

스펙은 sender가 local/remote `normalizedEffectiveMaxMessageBytes`의 작은 값을 admitted connection lifetime 동안 고정해 submit 전에 적용하도록 정한다(`common/internals/12-service-wire-protocol.ko.md:99-110`). ClientServer admission은 이 값을 보존하고 update가 바꾸지 못하게 막는다(`Runtime/Channels/ZLinkClientServerClientRuntime.cs:1454-1493`). 그러나 실제 send는 선택한 socket에 parts를 그대로 넘기고(`:136-151`), request도 같은 방식으로 `ZLinkRawRequestSubmitter`에 넘긴다(`:154-190`). Connection의 `_normalizedEffectiveMaxMessageBytes`는 hello/admit 검증에만 쓰이고 application submit 크기 검사에는 쓰이지 않는다. Server reply도 `Socket.Reply(...)`를 직접 호출한다(`:1511-1523`).

조사 당시 RouteMesh에는 반대로 complete-message 합계 검사가 있었지만 SIZE-001에서 제거했다. ClientServer에는
send/request/reply 각각에 exact admitted bound를 적용하고, remote 한도가 더 작은 경우와 negotiated
complete-message 경계의 바로 아래·위 사례를 production socket test로 검증해야 했다.

구현과 검증을 완료했다. Client connection은 admission이 반환한
`NormalizedEffectiveMaxMessageBytes`를 admitted lifetime의 불변값으로 읽고, send와 request를 native submit하기
전에 complete message의 모든 part byte를 합산한다. Server는 Hello에서 local·remote 중 작은 값을 peer별로
보관하고 inbound application message와 reply에 같은 값을 적용한다. Handler reply가 상한을 넘으면 원래
payload를 보내지 않고 같은 correlation의 `CapacityExceeded` error terminal을 상한 안에서 반환한다.

공통 `ZLinkClientServerMessageBound`가 send, request, receiver와 reply의 계산을 한 곳에서 소유한다. 512-byte
remote와 4 KiB local 조합에서 oversized send/request가 handler 실행 전에 `CapacityExceeded`로 끝나고,
oversized server reply도 caller가 같은 terminal을 관찰했다. 512 byte exact boundary는 허용하고 513 byte는
거절하는 회귀를 포함해 ClientServer 30건과 전체 .NET unit suite 1,576건이 통과했다. Packaged contract와
standalone HTTP clean consumer도 통과했으며 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`이다. 실제 process
`ChannelEgressRouting` `CH-REG-06`은 `logs/20260807-184635-466543/`에서 RouteMesh와 ClientServer request가
application retry 없이 각각 1초 안에 완료되는 것을 확인하고 통과했다.
이 checkpoint는 `d69e505ad2`로 `main`에 push했다.

### DOTNET-COMP-001 — completion retention 전체 포화 시 caller terminal 유실

**판정: 완료**

Internals는 early completion 보관 자리가 가득 차면 source runtime 소유 자원 부족을 caller가 `CapacityExceeded`로 관찰해야 하며, 응답을 버리고 timeout으로 바꾸는 것을 명시적으로 금지한다(`common/internals/04-completion.ko.md:110-127`). 현재 구현은 early payload store와 failure tombstone store 두 단계를 두지만, 둘 다 가득 차면 `OverflowCount`와 metric만 증가시키고 parts를 dispose한다(`Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:135-177`). 이후 같은 operation을 등록하면 overflow 사실을 찾을 표식이 없으므로 `_pending`에 들어가 timeout까지 기다린다(`:73-103`).

현재 test도 caller terminal을 확인하지 않는다. `CompletionTable_OverflowBeyondRetentionIsObservable`은 세 번째 완료 뒤 `OverflowCount == 1`만 주장한다(`UnitTests/Runtime/ServiceRuntimeFoundationTests.cs:1639-1651`). 이는 스펙의 “caller가 관찰 가능한 결과”보다 약한 assertion이다. Bounded 구조를 유지하되 등록 전 overflow operation이 나중에 반드시 `CapacityExceeded`로 끝나는 소유 구조가 필요하다.

구현과 검증을 완료했다. Early payload와 failure tombstone이 모두 포화되면 completion table은 retention
자원 전체를 fail-closed 상태로 전환한다. 이 전이는 table이 이미 소유한 pending callback을 모두
`Backpressured` terminal로 완료하고, 보관 중인 payload를 dispose하며, 이후 등록도 같은 terminal로 즉시
완료한다. 따라서 operation별 marker를 무한히 늘리지 않으면서 overflow terminal이 caller timeout으로
바뀌는 경로를 제거했다. Public 오류 변환에서는 source runtime의 이 terminal을 `CapacityExceeded`로
관찰한다.

Completion table focused test 6건은 overflow operation의 늦은 등록, 이미 pending인 operation과 overflow
이후 등록을 검증하고 통과했다. .NET unit project는 두 번 실행했으며 매번 1,576건이 통과했지만, 첫
실행에서는 `LogicalMulticastSubmitsEachPositiveRemoteOnceRegardlessOfWeight`, 두 번째 실행에서는
`RemoteUserSpotTerminalReplaysAfterDeadlineAndExpiresWithoutReexecution`이 각각 한 번 실패했다. 두 test는
각각 단독 재실행에서 통과했으므로 completion 변경과 별개인 간헐 실패로 분리한다. Packaged contract와
standalone HTTP clean consumer는 통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process
`ChannelEgressRouting` `CH-REG-06`은 `logs/20260807-190203-1199732/`에서 통과했다. 구현 checkpoint는
`d91088d688`로 `main`에 push했다.

### DOTNET-EXEC-001 — STREAM session execution queue가 payload byte를 세지 않음

**판정: 완료**

Internals는 실행 owner의 각 lane이 count와 byte를 모두 예약하고 payload 외에 작업당 고정 비용도 포함하도록 정한다(`common/internals/08-object-lifecycle.ko.md:219-268`). `ZLinkSerialExecutionQueue.TryPostAccepted`는 Spot ingress에서 `payloadLength + 256`을 올바르게 예약한다(`Runtime/Execution/ZLinkSerialExecutionQueue.cs:293-378`).

반면 STREAM session은 application packet의 header/payload를 closure로 capture한 뒤 payload 길이 없이 `EnqueueApplication`을 호출한다(`Runtime/Streams/ZLinkStreamSessionRuntime.cs:249-274`). Executor는 이를 payload-aware overload가 아닌 `TryPostApplicationWithAdmission`으로 넘기고(`Runtime/Streams/ZLinkStreamSessionSerialExecutor.cs:79-83`), queue는 항상 고정 256 byte만 예약한다(`Runtime/Execution/ZLinkSerialExecutionQueue.cs:179-207`). 새 session을 만드는 node-level ingress도 header/payload/lease를 capture하면서 같은 고정비 전용 executor에 넣는다(`Runtime/Streams/ZLinkStreamNodeRuntime.cs:663-742`). Host-wide inbound HWM lease가 payload를 세더라도 session execution queue의 독립 byte 한도를 대신하지 못한다. 큰 packet이 한 session 또는 session-creation ingress에 몰리면 64 MiB lane 상한이 아니라 4,096건 count 한도가 먼저 적용될 수 있다.

Application packet과 session-creation ingress 모두 complete retained bytes를 넘겨 예약하고 handler terminal에서 반납해야 한다. 작은 packet count 포화와 큰 packet byte 포화를 따로 재현하는 test가 필요하다.

구현과 검증을 완료했다. `ZLinkSerialExecutionQueue`의 application admission이 retained byte를 받아 작업당
고정비 256 byte와 함께 count·byte를 한 번에 예약하며, 기존 work item completion 경로에서 같은
`AccountingBytes`를 반납한다. 기존 STREAM session과 session 생성 ingress는 모두 header와 payload 크기의
합을 공통 계산 함수로 구한 뒤 이 admission에 전달한다. Host-wide inbound lease와 session execution
reservation의 책임은 합치지 않았으며 각 owner가 자신의 수명 동안 독립적으로 byte를 보유한다.

Byte exact-boundary, active work 중 byte 포화, completion 뒤 재수락과 STREAM cleanup을 포함한 focused test
25건과 전체 .NET unit suite 1,580건이 통과했다. Packaged contract와 standalone HTTP clean consumer도
통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 STREAM
연결·request를 포함하는 `ChannelEgressRouting` `CH-REG-02`는
`logs/20260807-191555-1965910/`에서 통과했다. 이 checkpoint는 `fdca4650c2`로 `main`에 push했다.

### DOTNET-EXEC-002 — 직렬 실행 engine이 공통 기관 하나로 수렴하지 않음

**판정: 완료**

Internals는 Spot, session과 Actor 전달의 순서·수락·준비 집합을 다루는 실행 engine을 하나만 두고, 자리별 차이는 별도 engine이 아니라 유효 상태만 표현하는 lane policy type으로 모델링하도록 정한다(`common/internals/09-session-binding.ko.md:33-59`). 확인 기준에도 “직렬 실행 원시 타입이 runtime 안에서 하나”라고 명시한다(`:115-124`).

Spot과 session은 `ZLinkSerialExecutionQueue`를 공통으로 사용한다(`Runtime/Spots/ZLinkSpotSerialExecutor.cs:5-16,55-60`, `Runtime/Streams/ZLinkStreamSessionSerialExecutor.cs:3-23`). 그러나 Actor dispatch는 별도 `ZLinkActorDispatchMailbox`가 ordinary/barrier waiter queue, admission close와 ready handoff를 자체 구현한다(`Runtime/Actors/ZLinkActorDispatchMailbox.cs:3-115`). Message Follow Actor 전달도 자체 `ConcurrentQueue`, count·byte admission과 drain ownership을 가진 `ActorQueue`를 별도로 구현한다(`Runtime/Spots/ZLinkActorMessageFollower.cs:777-845`). 각각이 개별 경로에서 맞게 동작하더라도 공통 engine 수정이 이 경로들에 자동 적용되지 않으므로 스펙이 금지한 중복 기관 구조다.

이 항목은 DOTNET-EXEC-001의 payload byte 누락과 다르다. 완료 조건은 공통 engine 하나에 Spot/session/Actor-delivery lane policy를 주입하고, 불가능한 lifecycle 조합을 타입으로 만들 수 없게 하는 것이다. FIFO, barrier, admission close, count·byte bound와 drain race test를 동일 engine contract suite로 실행해야 한다.

구현과 검증을 완료했다. Actor dispatch mailbox는 자체 waiter queue와 busy/drain 선택을 제거하고 공통
`ZLinkSerialExecutionQueue` 위에 Actor lifecycle 정책만 남겼다. 일반 dispatch와 terminal barrier는
application lane을 사용하고, 이미 대기 중인 일반 dispatch보다 먼저 실행해야 하는 deferred Join barrier는
`ZLinkSerialWorkLane.Lifecycle`을 사용한다. Admission close/reopen과 pending request는 Actor aggregate의
상태로 유지하지만 FIFO, lane 선택, count·byte reservation과 drain은 공통 engine이 소유한다.

Message Follow의 Actor별 `ConcurrentQueue`, drain flag와 별도 count·byte counter도 제거했다. 각 route는
같은 공통 engine에 encoded payload byte를 넘기며, engine의 application-drained signal이 route queue의
안전한 retirement를 결정한다. 기존 전역 admission slot은 Message Follow 전체 호출 수를 제한하는 별도
owner이므로 유지했다. 사용되지 않던 mailbox의 pending-message counter와 인자도 함께 제거했다.

공통 engine, Actor FIFO·cancellation·barrier·terminal close/reopen·handoff와 Message Follow 경로 focused
test 260건이 통과했다. 전체 .NET unit project는 1,580건이 통과하고
`LogicalMulticastSubmitsEachPositiveRemoteOnceRegardlessOfWeight` 한 건이 5초 condition timeout으로
실패했지만, 같은 test의 단독 재실행은 통과했다. 이 간헐 실패는 Actor serial 변경과 별도로 유지한다.
Packaged contract와 standalone HTTP clean consumer는 통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process
`ToActorMessaging` `TA-A1`은 `logs/20260807-193610-3123003/`에서 통과했다. 이 checkpoint는
`a05ebdc421`로 `main`에 push했다.

### DOTNET-EXEC-003 — Entry Actor ingress가 count·byte 상한 없이 적재됨

**판정: 완료**

Internals는 실행 대기열마다 count와 byte reservation을 모두 강제하고 상한 없는 실행 대기열을 금지한다(`common/internals/08-object-lifecycle.ko.md:219-268`). Host-wide payload HWM은 process 수신 byte 회계이며 owner execution queue의 count·byte 한도를 대체하지 않는다.

Entry Spot Actor ingress는 `Channel.CreateUnbounded<ZLinkSpotActorFrameBatch>`를 사용한다(`Runtime/Spots/ZLinkEntrySpotDispatchPump.cs:18-24`). 수신한 batch는 별도 queue reservation 없이 `TryWrite`되고(`:153-177`), reader는 batch를 bounded lane에 넘기는 대신 Actor ID별 선행 `Task` continuation chain으로 계속 전환한다(`:180-203,234-249`). Batch가 가진 `ZLinkInboundDispatchLease`는 payload byte를 host-wide budget에 유지하지만(`Runtime/Spots/ZLinkSpotActorFrameReader.cs:86-117`) batch/task/envelope count와 고정비를 제한하지 않는다. 따라서 빈 payload나 작은 payload가 몰리면 host payload HWM 아래에서도 Channel item과 Task가 무제한 늘 수 있다.

완료 조건은 Entry Actor ingress를 count와 retained byte를 원자적으로 예약하는 공통 bounded lane으로 옮기고, 포화 위치와 call 종류에 맞는 terminal/admission 결과를 내는 것이다. Zero/small-payload count 포화와 large-payload byte 포화, Actor별 FIFO와 sibling Actor progress를 함께 검증해야 한다.

구현과 검증을 완료했다. Entry Actor ingress의 unbounded `Channel`과 Actor별 `Task` continuation chain을
제거했다. Process-wide ingress admission은 batch 수와 retained body·application metadata byte에 작업당
고정비 256 byte를 더해 원자적으로 예약한다. 각 Actor는 공통 `ZLinkSerialExecutionQueue`를 사용하므로
Actor별 count·byte 한도와 FIFO를 같은 execution engine이 소유하며, Actor마다 lane을 분리해 한 Actor의
handler가 대기해도 다른 Actor의 dispatch를 막지 않는다. Lane이 비면 공통 engine의 drained signal로
dictionary entry와 queue를 함께 정리한다.

Process-wide 또는 Actor lane admission이 포화되면 request는 `CapacityExceeded`와 retry-after-backoff로
끝나고, one-way message는 handler를 실행하지 않은 채 Framework가 보유한 payload를 반납한다. 종료 시에는
새 ingress를 닫고 이미 수락한 batch의 reservation과 dispatch가 모두 끝난 뒤 lane을 해제한다.

`EntrySpotActorDispatchTests` 136건에서 byte 상한 초과의 request terminal과 payload 반납, Actor별 count
포화, 같은 Actor의 후속 handler 미실행과 sibling Actor progress를 포함해 통과했다. 전체 .NET unit
project는 1,583건 중 1,582건이 통과하고 기존
`LogicalMulticastSubmitsEachPositiveRemoteOnceRegardlessOfWeight` 한 건이 5초 condition timeout으로
실패했지만 같은 test의 단독 재실행은 통과했다. Packaged contract와 standalone HTTP clean consumer가
통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process
`ToActorMessaging` `TA-A1`은 `logs/20260807-195320-4080230/`에서 통과했다. 이 checkpoint는
`45a3658fd8`로 `main`에 push했다.

### DOTNET-LIFE-001 — User/Instance Spot이 서로 다른 runtime type이 아님

**판정: 완료**

Internals는 Entry, User, Instance Spot의 생성·이동·idle 반환 규칙이 다르므로 세 종류를 서로 다른 타입으로 표현하고, 한 타입의 tag/interface 검사로 구분하지 않도록 정한다(`common/internals/08-object-lifecycle.ko.md:20-38`). 판정 기준은 상속·합성·tagged union 중 어떤 문법을 썼는지가 아니라 불가능한 종류·기능 조합을 만들 수 있는지다.

Entry Spot은 `ZLinkEntrySpotActivation`으로 분리되어 있지만 User와 Instance는 같은 `ZLinkSpotActivation`이 `IZLinkSpotContext`와 `IZLinkInstanceSpotContext`를 동시에 구현한다(`Runtime/Spots/ZLinkSpotActivation.cs:14-19`). 실제 종류는 내부 `Spot` 객체가 `IZLinkInstanceSpot`인지 반복 검사해 가른다(`Runtime/Spots/ZLinkSpotActivationConfiguration.cs:121-166`, `Runtime/Spots/ZLinkSpotActivationExecution.cs:439-448,790`, `Runtime/Spots/ZLinkSpotNodeCatalog.cs:1137-1149,1173-1184`). 공통 타입에는 User 전용 relocation-ready와 Instance 전용 close/handler surface가 함께 존재해 유효 조합을 호출부가 알아야 한다.

완료 조건은 공통 resource ownership을 base/component로 공유하되 User/Instance activation과 허용 lifecycle operation을 타입 경계에서 분리하는 것이다. Instance에 User-only relocation-ready를, User에 Instance-only idle close를 구성할 수 없음을 compile-time 또는 closed-union exhaustive test로 검증해야 한다.

구현과 검증을 완료했다. 공통 socket·scope·timer·serial executor·payload ownership은 abstract
`ZLinkSpotActivation`이 유지하고, factory는 `ZLinkUserSpotActivation`과
`ZLinkInstanceSpotActivation`을 각각 만든다. User activation만 `IZLinkSpotContext`와 전체 handler registry,
Actor leave와 relocation-ready operation을 제공한다. Instance activation은
`IZLinkInstanceSpotContext`와 packet handler registry, Instance initialize·close lifecycle만 제공한다.
따라서 하나의 activation에 두 context interface나 두 handler capability를 함께 구성할 수 없다.

Configuration descriptor bind, scanned handler 허용 범위, closing callback과 Instance initialization은 subtype의
override가 소유한다. Catalog와 retire scheduler가 application `Spot` 객체의 interface를 반복 검사하던 분기도
activation이 제공하는 kind·placement policy로 바꿨다. Instance evidence wait runner는 첫 번째 node의 10초
HTTP timeout 때문에 실제 owner인 두 번째 node를 조회하지 못하던 문제를 함께 고쳐, 두 node의 공개 evidence를
bounded polling한다.

Runtime type·context·handler capability의 상호 배타성과 User/Instance lifecycle을 포함한 focused test 157건이
통과했다. 전체 .NET unit project는 1,584건 중 1,583건이 통과하고 기존
`LogicalMulticastSubmitsEachPositiveRemoteOnceRegardlessOfWeight` 한 건이 5초 condition timeout으로
실패했지만 같은 test의 단독 재실행은 통과했다. Packaged contract와 standalone HTTP clean consumer가
통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process
`InstanceSpot` `IS-E2E-01`은 `SpotService/logs/20260807-201508-822914/`에서 cold activation,
initialization 1회와 request handler 1회를 확인하고 통과했다. 이 checkpoint는 `4e4b2a626d`로 `main`에
push했다.

### DOTNET-LIFE-002 — Ready Instance owner loss의 process 증거 누락

**판정: 완료**

공개 계약인 failure spec §4.4는 `Ready` authority의 owner process 종료나 lease 만료를 Missing으로 바꾸지 않고,
다른 node의 cold activation 없이 call을 bounded `Unavailable`로 끝내도록 요구한다. Relocation Store의
activation record는 같은 target node와 lifecycle에서 끝나지 않은 initial cold activation만 재개한다
(`common/spec/31-failure-failover-policy.ko.md`, .NET exact interface
`server/languages/dotnet/interfaces/05-spots.ko.md`).

Internals 06·08·10은 이 결과를 resolver의 닫힌 결과, activation state와 liveness 책임 분리로 구현하고,
12는 same-target initial recovery root와 scan을 설명한다. 이 구조 문서들은 공개 오류나 failover 범위를
추가하지 않는다.

현재 source는 이 의미를 이미 구분한다. Instance address lookup은
`ResolveSpotRowWithStatusAsync(...)`의 `KnownUnavailable`을 받으면 `Unavailable`을 던지고 cold activation에
넘기지 않는다 (`Runtime/Host/ZLinkFrameworkRuntimeLocationAccess.cs:39-46`). Unit test도 lease 만료 뒤
row가 null이어도 resolution kind가 `KnownUnavailable`인지 확인한다
(`UnitTests/Runtime/LocationResolverTests.cs:243-263`). 따라서 source 수준의 새 구현 GAP으로 판정하지
않는다.

조사 당시 남은 조건은 실제 process 증거였다. `IS-E2E-05`와 `IS-E2E-35`가 미구현이어서 owner process 종료,
lease invalidation, bounded `Unavailable`, 새 factory·handler 실행 부재와 자동 queue recovery 부재를
검증하지 못했다 (`framework/languages/dotnet/e2e/InstanceSpot/feature-map.ko.md:17,47`). 이 두 E2E와
같은 target node/lifecycle의 검증되지 않은 initial cold activation recovery positive scenario가 필요했다.

요구한 process 검증을 구현하고 통과했다. `IS-E2E-05`는 public location query로 Ready owner와
`ObjectGeneration`을 확정한 뒤 실제 owner process를 SIGKILL한다. Owner lease 무효화 뒤 public location은
같은 generation의 `Unavailable`이 되었고, 후속 Instance intent request도 `Unavailable`로 한 번 끝났다.
다른 owner의 handler와 추가 factory initialization은 모두 0건이었다. 실행 log는
`SpotService/logs/20260807-202307-1312454/`다.

`IS-E2E-35`는 Ready Spot의 first handler를 Application gate에서 대기시키고 follow-up request를 같은 queue에
넣은 뒤 owner를 SIGKILL하고 같은 role을 restart했다. 두 caller는 각각 `ShuttingDown`과
`DeadlineExceeded` terminal을 받았으며, queued operation의 handler replay는 0건이었다. Restart 뒤 public
location과 후속 request는 같은 generation의 `Unavailable`이고 factory initialization은 최초 1건뿐이었다.
실행 log는 `SpotService/logs/20260807-202604-1475569/`다.

Ready owner failover와 구분할 positive control도 추가했다. Initial `OnInitializeAsync`를 gate로 대기시켜
public location이 `Creating`인 동안 두 번째 request를 보냈고, 같은 target·generation에 합류한 뒤 gate를
해제했다. Initialization은 한 번, 두 operation handler는 각각 한 번 실행되고 Ready publication까지
generation이 유지되었다. 실행 log는 `SpotService/logs/20260807-202813-1625005/`다. 기존 Track A도
`SpotService/logs/20260807-202935-1718197/`에서 다시 통과했다. 이 E2E checkpoint는
`c797108cce`로 `main`에 push했다.

### DOTNET-COMP-002 — submit 뒤 waiter 등록 구조 유지

**판정: 완료**

Internals는 응답 상관 값을 sender가 먼저 만들고 waiter를 등록한 다음 submit하도록 정한다. Submit이 operation ID를 출력하면 same-process 응답이 등록보다 먼저 도착해 early-result table이 필요해지므로 이 구조 자체를 제거 대상으로 명시한다(`common/internals/04-completion.ko.md:60-105`).

`.NET` wrapper는 `_spot.RequestToSpot(..., out var operationId, ...)`를 먼저 호출한 다음 `_completions.RegisterRequest(...)`를 호출한다(`Runtime/Backend/DotNet/Wrappers/ZLinkBackendSpotWrapper.cs:217-231`). `ZLinkMeshCompletionTable` 주석과 `_early` map도 이 경쟁을 정상 구조로 전제한다(`Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:6-13,69-72`). DOTNET-COMP-001은 이 구조가 만든 유한 보관 자원에서 실제 terminal 유실로 이어진다.

완료 조건은 Framework가 correlation ID를 먼저 할당해 input으로 전달하는 binding/public surface와 register-before-submit 회귀 test다. Early store 크기를 늘리는 것은 gap을 닫지 않는다.

구현과 검증을 완료했다. Framework backend는 node가 만든 응답 상관 값을 completion table에 먼저
등록한 뒤 managed submit의 입력으로 전달한다. Spot·Message Follow·Actor request와 join·Instance Spot
activation·remote object operation·STREAM bind/unbind가 같은 `RegisterBeforeSubmit` 경로를 사용한다.
동기 submit이 거절되거나 예외를 던지면 같은 경계에서 waiter를 제거한다. 따라서 dispatch pump가
등록되지 않은 terminal을 먼저 받는 정상 경로가 사라졌고, early payload store와 tombstone store도
제거했다.

회귀 test는 submit callback 안에서 completion을 동기 발생시켜 waiter가 이미 실행 가능한 상태인지
확인하고, submit 거절 뒤 늦게 도착한 payload는 handler에 연결하지 않고 dispose하는지 확인한다.
Completion table focused test 4건과 전체 .NET unit suite 1,582건이 통과했다. 관련 test 묶음에서
`RemoteUserSpotTerminalReplaysAfterDeadlineAndExpiresWithoutReexecution`이 한 번 실패했지만 같은 test의
단독 재실행과 전체 suite 재실행은 통과했다. Packaged contract와 standalone HTTP clean consumer도
통과했으며 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process는
ToActor `TA-A1`을 `ToActorMessaging/logs/20260807-205106-2541281/`에서, Instance Spot Track A를
`SpotService/logs/20260807-205128-2543009/`에서 통과했다. 구현 checkpoint는 `4005fe5c8e`로
`main`에 push했다.

### DOTNET-LAYER-001 — 의미 없는 binding pass-through 계층

**판정: 완료**

Internals의 POSDDD 관문은 binding API를 그대로 복제하는 adapter와 test fake만을 이유로 둔 단일 구현 `IBackend*`를 금지한다(`common/internals/01-layering.ko.md:80-124`). 조사 당시 publisher wrapper는 bind, socket option, send-ready, publish와 dispose를 같은 인자·결과로 전달할 뿐 Framework 의미를 숨기지 않았다(`Runtime/Backend/DotNet/Wrappers/ZLinkBackendPublisherSocketWrapper.cs:3-61`). Router wrapper도 option과 send/recv 대부분을 동일하게 전달했다(`Runtime/Backend/DotNet/Wrappers/ZLinkBackendRouterSocketWrapper.cs:3-167`). 대응 `IZLinkBackend*` interface는 binding surface를 다시 선언했고(`Runtime/Backend/Contracts/IZLinkBackendSocketContracts.cs:9-176`), production 구현은 각각 이 wrapper 하나뿐이었다.

MeshNode/Spot/Stream처럼 ownership, pull-dispatch, completion과 fencing을 결합하는 adapter까지 제거하라는 뜻은 아니다. Socket별 pass-through interface/wrapper를 직접 binding public API 사용으로 바꾸고, 실제 의미 변환만 좁은 adapter에 남겨야 한다. 변경 전후 throughput, p99, allocation과 lock contention을 측정해야 이 구조 gap을 완료할 수 있다.

일반 DEALER, ROUTER, PUB, SUB의 `IZLinkBackend*Socket`과 단일 production wrapper를 제거했다. Runtime
context와 Channel 실행 경로는 binding의 public socket interface를 직접 사용한다. Socket option 변환,
poller와 monitor 연결은 Framework 정책을 적용하는 좁은 adapter로 유지했고, MeshNode·Spot·STREAM adapter는
ownership, completion과 lifecycle 의미를 결합하므로 유지했다. Channel bundle은 socket interface를 다시
선언하지 않고 dispose와 manual connection ownership만 관리하며, 사용되지 않던 automatic connection
bookkeeping도 제거했다. Test는 binding surface를 복제한 fake 대신 실제 socket과 runtime이 기록하는
socket 생성 횟수·monitoring 상태를 사용한다.

같은 host에서 1 KiB payload, request window 100, warmup 1,000회, active 30초 조건으로 변경 전
`40255cd707`과 변경 뒤를 차례로 측정했다. Throughput은 23.05 KOPS에서 27.08 KOPS로 17.49% 증가했고,
mean latency는 3.673 ms에서 3.337 ms로, p99는 101.018 ms에서 99.790 ms로 낮아졌다. `System.Runtime`
counter의 server allocation은 초당 613,769,509 byte에서 676,926,534 byte로, monitor lock contention은
초당 9,761.625회에서 10,812.538회로 늘었다. 처리량 차이를 반영해 완료 1건당 환산하면 allocation은
26,625.74 byte에서 24,994.55 byte로 6.13%, contention은 0.423466회에서 0.399238회로 5.72% 감소했다.
Counter 표본은 변경 전 8개, 변경 뒤 13개이며 throughput·latency와 같은 active 구간에서 수집했다.

관련 owner-layer test 85건과 전체 .NET unit suite 1,582건이 통과했다. Packaged contract와 standalone
HTTP clean consumer가 통과했고
public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process는
PubSub `PS-A1`을 `PubSub/logs/20260807-211921-3611470/`에서, ChannelEgressRouting `CH-E2E-01`을
`ChannelEgressRouting/logs/20260807-211942-3612322/`에서 통과했다. 구현 checkpoint는
`8ad6e969cc`로 `main`에 push했다.

### DOTNET-LAYER-002 — runtime 종료 의미가 ASP.NET Core 통합 package에 있음

**판정: 완료**

Internals는 host 통합 계층이 runtime 시작·종료를 host lifecycle에 **연결만** 하고, 수락 중지·drain·relocate·close의 의미와 순서는 runtime이 소유해야 한다고 정한다(`common/internals/01-layering.ko.md:150-173`). .NET package 계약도 `Zlink.Framework`가 location runtime을 소유하고 `Zlink.Framework.AspNetCore`는 DI 등록과 host lifecycle 연결만 담당한다고 구분한다(`interfaces/02-configuration-host.ko.md:12-21`).

실제 public `IZLinkFrameworkRuntime`의 유일한 production 구현은 ASP.NET Core package의 `ZLinkFrameworkMaintenanceRuntime`이다(`Zlink.Framework.AspNetCore/ZLinkFrameworkMaintenanceRuntime.cs:8-44`). 이 타입이 runtime state, relocate/shutdown operation, deadline, observer와 drain coordinator를 직접 소유한다. DI도 이 타입을 `IZLinkFrameworkRuntime`으로 등록한다(`Zlink.Framework.AspNetCore/ZLinkFrameworkServiceRegistrar.cs:145-165`). 반면 core package의 `Runtime/Host/ZLinkFrameworkRuntime`은 Spot manager와 내부 resource를 소유하지만 public maintenance runtime을 구현하지 않는다(`Runtime/Host/ZLinkFrameworkRuntime.cs:31-78`). 따라서 ASP.NET Core 통합 없이 같은 종료·재배치 의미를 조립할 수 없으며 package 책임도 exact 문서와 반대다.

완료 조건은 maintenance state machine과 종료·재배치 순서를 `Zlink.Framework`로 옮기고, ASP.NET Core package에는 hosted-service 연결만 남기는 것이다. Console/test host에서도 같은 runtime API로 동일한 terminal-once와 resource close 순서를 검증해야 한다.

Maintenance runtime, drain coordinator와 executor, auto-connect lifecycle을 `Zlink.Framework`의
`Runtime.Host`로 옮겼다. 새 `ZLinkFrameworkHostRuntimeCoordinator`가 location 준비, Framework 시작,
RouteMesh monitoring과 auto-connect 시작, shutdown drain, owner row 정리와 resource close 순서를 소유한다.
ASP.NET Core의 `ZLinkFrameworkHostedService`는 host의 `StartAsync`와 `StopAsync`를 이 coordinator에 전달하는
역할만 남겼다. 따라서 relocation·shutdown state와 close 순서는 ASP.NET Core assembly를 참조하지 않고도
Core runtime test host에서 조립하고 실행할 수 있다.

Core assembly ownership과 ASP.NET Core assembly에서 이전 구현 type이 제거됐음을 확인하고, ASP.NET host
없이 조립한 maintenance runtime에서 반복 shutdown이 같은 terminal을 반환하며 drain 실행과 shutdown
요청을 한 번만 수행하는 회귀를 추가했다. Maintenance, drain과 auto-connect focused test 71건과 전체
.NET unit suite 1,584건이 통과했다. Packaged contract와 standalone HTTP clean consumer도 통과했고
public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process는
restore artifact를 준비한 뒤 ResilienceLifecycle `RL-B4`를
`ResilienceLifecycle/logs/20260807-213609-146114/`에서 통과해 runtime drain terminal과 drain 뒤 신규 요청
차단을 확인했다. 구현 checkpoint는 `9bdb5a0dc7`로 `main`에 push했다.

### DOTNET-LAYER-003 — 수명이 다른 식별자를 내부에서도 `string`으로 혼용

**판정: 완료**

Internals는 mesh 이름, node RID, channel 이름, object ID처럼 범위와 수명이 다른 식별자를 각각 전용 타입으로 두고 문자열 변환은 경계에서 한 번만 하도록 정한다(`common/internals/01-layering.ko.md:317-359`). Public exact API가 application 편의를 위해 `string`을 받는 것과 runtime 내부 표현까지 문자열이어야 한다는 것은 다른 문제다.

초기 audit에서는 Actor runtime, Spot activation과 registry의 여러 내부 값이 문자열이었다. 현재는 주요 registry와 session binding의 저장 key를 value type으로 바꾸었고, Actor ownership의 core lifecycle method와 relocation session state도 typed parameter를 사용한다. 2026-08-08 후속 수정에서는 relocation application actor snapshot, source capture·sealed route map과 drain target map도 `ZLinkActorId`·`RoutingId` key로 고정하고, retire preflight의 mesh·RID 복합 key는 tuple value로 보관한다. 남은 문자열은 명시적인 호환 overload와 codec·Store·diagnostic serialization 경계로 분리해 확인해야 한다.

완료 조건은 public/binding/store 경계에서 한 번 validation·변환하고 runtime key와 method parameter에는 `MeshName`, `ChannelName`, `ActorId`, `SpotId` 등 구분된 value type을 사용하는 것이다. 동일 문자열 값이 서로 다른 identifier domain에 있어도 섞이지 않는 compile/runtime test가 필요하다.

`ZLinkMeshName`, `ZLinkChannelName`, `ZLinkActorId`, `ZLinkSpotNodeName`, `ZLinkStreamNodeName`,
`ZLinkTimerName`을 runtime 전용 value type으로 두고, Spot ID validation helper는 `ZLinkSpotId`로
변환한다. Actor runtime state와 session binding record, ClientServer client runtime, Spot activation은
문자열을 경계에서 변환한 뒤 전용 타입으로 보관한다. Component state의 channel·SpotNode·StreamNode registry,
RouteMesh channel index, Spot executor의 Actor·timer lane도 올바른 key type을 사용한다. Actor session registry와
Spot catalog의 active·pending·closing key도 각각 `ZLinkActorId`와 `ZLinkSpotId`를 사용한다. ClientServer의
`_connections` key는 channel ID가 아니라 `manual/local/auto` connection identity이므로 channel ID로 분류하지
않고 별도의 `ZLinkChannelName` field로 channel domain을 고정한다.

2026-08-08 후속 수정으로 Actor ownership `_actors`, session Actor operation gate, Spot Actor membership,
ordered relay tail과 mesh field, bound-session registry의 session key, per-Spot dispatch registry, Actor remote
frame FIFO chain과 Spot subscription tracker는 각각 `ZLinkActorId`, `ZLinkMeshName`, `ZLinkSpotId` 또는
`RoutingId`로 바꾸고 경계 변환 regression을 추가했다
(`Runtime/Locations/ZLinkActorOwnershipCoordinator.cs:37`, `Runtime/Streams/ZLinkSessionActorCoordinator.cs:15`,
`Runtime/Spots/ZLinkSpotActorMembership.cs:7`, `Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:39-50`,
`Runtime/Locations/ZLinkSpotLocationLifecycle.cs:9`, `Runtime/Host/ZLinkActorBoundSessionRegistry.cs:5`,
`Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:27`, `Runtime/Host/ZLinkFrameworkRuntimeActors.cs:3905`,
`Runtime/Backend/DotNet/Wrappers/ZLinkSpotSubscriptionTracker.cs:11`).
Production build는 warning/error 0이다. Actor ownership의 claim·authority·release core method는
`ZLinkActorId`·`ZLinkMeshName`·`ZLinkSpotId`를 받고, 문자열 overload는 legacy test/configuration boundary에서
typed core로 한 번 변환한다(`Runtime/Locations/IZLinkActorLocationLifecycle.cs:1-30`,
`Runtime/Locations/ZLinkActorOwnershipCoordinator.cs:39-120,195-349,351-799,917-1166,1324-1661`).

추가로 session binding table의 `_entries`와 `_tombstones`는 `ZLinkSessionBindingKey.ActorId`를
`ZLinkActorId`로 저장하고 `FromBoundary`에서만 문자열을 변환한다
(`Runtime/Actors/ZLinkSessionActorBindingTable.cs:161-184,203-234`). Session binding route와 confirmed
identity도 `MeshName`을 `ZLinkMeshName`으로 보관하고 route fence는 typed mesh를 비교하며, binding table의
core `Bind`는 `ZLinkActorId`를 받는다(`Runtime/Actors/ZLinkSessionActorBindingTable.cs:27-120,203-218`,
`Runtime/Streams/ZLinkSessionActorCoordinator.cs:52-62,542-626,745-755`). Actor session registry의
`GetOrCreate`·`TryGet`·`TryRemove`·`RemoveIfCurrent`도 `ZLinkActorId`를 받고
(`Runtime/Actors/ZLinkActorSessionRegistry.cs:11-48`), Actor session binding의 `BindSession`·
`BeginSessionReplacement`와 Spot lifecycle의 Claim/relocation/release method는 `ZLinkMeshName`·`ZLinkSpotId`를
받는다(`Runtime/Actors/ZLinkActorRuntimeState.cs:385-508`, `Runtime/Locations/ZLinkSpotLocationLifecycle.cs:12-158`).
`RuntimeIdentifierTests`는 이 method parameter와 Actor key, 두 binding identity의 domain, Actor ownership
core lifecycle parameter를 확인한다(`RuntimeIdentifierTests.cs:158-236`). 현재 source build는
warning/error 0이고 identifier·replacement focused test는 net8.0과 net10.0 Release에서 각각 235/235로
통과했다. 이 수정으로 기존 registry와 Spot lifecycle residual 및 Actor ownership core parameter residual은
해소했지만, 전체 언어별 release matrix와 package/process evidence는 별도 계약 gate로 남아 있다.

최신 동일 Codex Sol 재검토에서 지적된 두 production owner residual도 spec-neutral typed boundary로 전환했다.
`ZLinkLocationAutoConnectHost`의 RouteMesh/local loop key와 `_meshName`은 `ZLinkMeshName`으로 보관하고
(`Runtime/Locations/ZLinkLocationAutoConnectHost.cs:31`, `Runtime/Locations/ZLinkAutoConnectLoop.cs`),
`ZLinkBoundedRemoteRequestAdmission`의 dictionary와 method parameter는 `ZLinkSessionBindingKey`로 고정했다
(`Runtime/Host/ZLinkBoundedRemoteRequestAdmission.cs:9`). 문자열은 repository·descriptor·wire·diagnostic처럼
허용된 boundary에서만 변환한다. 동일 Sol은 configuration·Store/wire serialization·endpoint·packet/handler
name·metadata·opaque binding token 문자열을 허용된 경계로 재분류했고 새 production identifier-owner residual을
찾지 못했다.

value type 사이에는 implicit·explicit conversion이 없으며, 같은 문자열을 Actor ID와 Spot ID로 만들어도 서로
다른 dictionary key type으로만 사용할 수 있음을 `RuntimeIdentifierTests`가 확인한다. 최신 source production
build는 warning/error `0/0`, owner/lifecycle focused gate는 net8/net10 각각 `90/90`, full Release aggregate는
각각 `1,622/1,622`로 통과했다 (`/tmp/zlink-dotnet-build-net8-typed-owner-residual.log`,
`/tmp/zlink-dotnet-focused-net8-typed-owner-residual.log`,
`/tmp/zlink-dotnet-focused-net10-typed-owner-residual.log`,
`/tmp/zlink-dotnet-unit-net8-20260808-typed-owner-residual.log`,
`/tmp/zlink-dotnet-unit-net10-20260808-typed-owner-residual.log`). Strict package verifier는 9개 package와
standalone HTTP clean consumer, API snapshot `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`
를 통과했고 XML snapshot hash는 `19288e149fadd087791bbc38f730d7a906fd3d66ac3c36f7aef50207d872c42b`이다
(`/tmp/zlink-dotnet-package-typed-owner-residual-strict.log`). 최신 ST-E2는
`SpotActorTransfer/logs/20260808-212305-2229477/`에서 callback guidance-before-close를 확인했고, TA-A4는
`ToActorMessaging/logs/20260808-212328-2239176/`에서 `to-actor-messaging e2e result=passed`를 확인했다.
따라서 `DOTNET-LAYER-003` production owner-key residual은 완료로 판정한다. Windows/6-RID와 전체 process
release gate는 이 identifier 항목과 독립된 미완료 조건으로 계속 추적한다.

### DOTNET-LAYER-004 — STREAM client와 server가 같은 protocol stack을 중복 구현

**판정: 완료**

Internals는 client 접속 library와 Framework가 같은 protocol을 별도 구현하지 않고 protocol 처리 한 곳을 양쪽이 사용하도록 정한다(`common/internals/01-layering.ko.md:175-181`). 이는 public client/server API를 합치라는 뜻이 아니라 wire header, correlation, pending request, lifecycle/close 같은 공통 protocol mechanism의 정본을 하나로 두라는 결정이다.

현재 `Systems.Zlink.Stream.Connector`는 자체 `ZlinkStreamHeaderCodec`, metadata/closing codec, correlation, pending-request table, receive loop와 lifecycle을 구현한다(`Systems.Zlink.Stream.Connector/Runtime/Protocol/`, `Runtime/ZlinkStreamPendingRequests.cs`, `Runtime/ZlinkStreamConnectorLifecycle.cs`). Framework server에는 별도의 `ZLinkStreamHeaderCodec`, `ZLinkStreamSessionClosingCodec`, frame reader/writer, session liveness와 request/reply 경로가 있다(`Zlink.Framework/Runtime/Streams/`). 실제로 같은 request sequence·header validation·closing record를 양쪽 소스에서 각각 유지하므로 한쪽 수정이 다른 쪽에 자동 반영되지 않는다.

완료 조건은 transport와 client/server orchestration은 분리하되 공통 wire codec, frozen validation과 correlation primitives를 shared internal protocol package로 옮기는 것이다. 기존 client-server golden/negative fixture를 shared codec에 한 번 적용하고 connector와 Framework 양쪽 clean consumer가 그 package를 사용하는지 확인해야 한다.

재검토 시 header와 frame codec, correlation ID 생성은 이미
`Systems.Zlink.Stream.Connector.Runtime.Protocol`의 internal 구현을 Framework가 참조하고 있었다. Client의
pending request table과 server의 inbound request/session table은 역할과 상태 전이가 달라 같은 primitive의
중복이 아니었다. 실제로 별도 구현이 남아 있던 `session-closing` encode를 connector protocol codec으로
옮기고 Framework codec 파일을 삭제했다. 이제 client decode와 server encode가 같은 version, reason,
diagnostic length와 strict UTF-8 validation을 사용한다.

Shared codec golden·negative 10건, Framework wire/drain 13건과 Connector 전체 147건이 통과했다. Framework
전체 unit suite는 1,586/1,587건이 통과했고, 실패한 기존 timing-sensitive test는 단독 재실행에서
통과했다. Packaged contract와 standalone HTTP clean consumer도 통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process는
ResilienceLifecycle `RL-B4`를 `ResilienceLifecycle/logs/20260807-222236-2437559/`에서 통과해 server drain
closing reason이 connector client까지 전달되는 경로를 확인했다. 구현 checkpoint는 `93d889336d`로
`main`에 push했다.

### DOTNET-OWN-001 — 소유한 payload를 public copying factory로 다시 복사

**판정: 완료**

Internals는 binding이 강제하지 않은 framework full-buffer copy를 0으로 줄이고, public immutable payload의 안전성은 유지하되 runtime 내부 소유권 이전에는 복사하지 않는 경로를 별도로 두도록 정한다(`common/internals/11-message-ownership.ko.md:19-45,95-98`).

`ZLinkEncodedPayload.From(...)`의 세 overload는 public caller buffer를 보호하기 위해 모두 `ToArray()`로 복사한다(`Zlink.Framework.Contracts/Codecs/ZLinkEncodedPayload.cs:10-31`). 이 public 동작 자체는 맞다. 문제는 runtime도 이미 자신이 소유한 memory에 같은 factory를 사용한다는 점이다. JSON 송신은 `SerializeToUtf8Bytes`가 새 배열을 만든 직후 `From(byte[])`으로 전체를 한 번 더 복사한다(`Runtime/Messaging/ZLinkMessageRuntime.cs:128-143`). Custom codec 수신도 runtime-owned `_payload`와 STREAM packet memory를 `From(span)`으로 다시 복사한 뒤 serializer에 넘긴다(`Runtime/Messaging/ZLinkMessageRuntime.cs:112-115`, `Runtime/Streams/ZLinkStreamPacketPayloadCodec.cs:56-60`). 이는 source에서 확인한 추가 full-buffer copy이며 실제 throughput/p99 영향 크기는 아직 benchmark하지 않았다.

완료 조건은 public factory의 defensive copy는 유지하면서 friend/internal owned-memory factory 또는 ownership token을 추가하는 것이다. JSON encode와 custom serializer decode에서 payload 크기만큼의 추가 배열·copy가 사라졌음을 allocation/copy benchmark와 buffer lifetime test로 확인해야 한다.

Public `From(...)`의 defensive copy 계약은 유지하고 friend assembly만 호출할 수 있는 `FromOwned(...)`를
추가했다. Framework JSON encode, envelope와 STREAM custom serializer decode, HTTP client custom serializer
decode는 runtime이 소유한 memory를 이 경로로 이전한다. `FromEncoded(...)`와 STREAM packet decode도 중간
`ToArray()`를 제거해 payload lifetime을 `ZLinkMessage`가 직접 유지한다.

16 KiB payload를 128회 전달한 allocation regression에서 owned 경로는 0 B, public copying 경로는
2,100,224 B를 할당했다. 같은 test가 public factory의 caller mutation 격리와 owned factory의 동일 buffer
retention도 검증한다. Framework 전체 unit suite 1,590건과 HTTP client unit 63건이 통과했다. Packaged
contract와 standalone HTTP clean consumer가 통과했고 public API snapshot hash는
`c1987f4b98e4fac7a30b7d038a56ee0d20e1272d00e79bdc88d3271e3c3ab958`로 유지되었다. 실제 process는
RegistrationCodec `RC-B1`~`RC-B4`를 `RegistrationCodec/logs/20260807-223341-3019087/`에서 통과해 JSON,
Protobuf, MessagePack과 codec coexistence 경로를 확인했다. 구현 checkpoint는 `07ab6afcce`로 `main`에
push했다.

### SPEC-TIMER-001 — non-catch-up timer option canonical normalization

**판정: 완료**

Exact interface는 `MaxCatchUpTicks`를 `CatchUpBounded`에서만 사용하고 `1..Int32.MaxValue`로 검증하며, 다른 overrun policy에서는 이 범위로 검증하지 않는다고 정한다. 또한 relocation은 `ZLinkTimerOptions`를 자동으로 포함한다(`interfaces/05-spots.ko.md:499-509`). 등록 경로는 실제로 `CatchUpBounded`일 때만 양수를 검사하므로 이 부분은 맞다(`Runtime/Spots/ZLinkSpotTimerRegistry.cs:333-352`).

Canonical relocation writer는 policy와 무관하게 `Math.Max(1, timer.Options.MaxCatchUpTicks)`를 기록하고
decoder는 0을 거부한다(`Runtime/Spots/ZLinkCanonicalSpotRelocationWriter.cs:128-132`,
`Runtime/Spots/ZLinkSpotTimerRelocationCodec.cs:118-131`). 최초 판정에서는 이를 exact interface와의 충돌로
보았지만, 공통 정식 spec은 non-catch-up policy에서 동작에 영향을 주지 않는 값을 relocation encoding이
유효한 기본값으로 normalize할 수 있다고 명시한다(`common/spec/05-async-execution-policy.ko.md:461-463`).
따라서 canonical schema의 `nonzero-u64`와 현재 writer·decoder는 이 normalization 계약에 맞는다.

Canonical encode, transport decode와 timer restore를 잇는 theory test를 추가했다. `SkipLateTicks(0)`과
`DelayNextTick(-3)`은 `1`로 normalize되고 `CatchUpBounded(7)`은 `7`로 보존되며 policy 자체는 세 경우 모두
유지된다. Focused 3건과 Framework 전체 1,593건이 통과했다. Service-wire decoder fixture validator와
contract test 76건도 통과했다. 실제 process는 timer overrun 동작을 SpotService
`logs/20260807-224140-3373181/`의 `sm-e4`에서 확인했다. Regression checkpoint는 `3f02183cb1`로 `main`에
push했다.

### DOTNET-SESS-REPLACE-001 — 이전 session 통지와 callback terminal 100 ms 뒤 close

**판정: BLOCKED (formal protocol 변경 승인 대기)**

승인된 계약은 replacement identity가 current로 등록되면 이전 owner의 ACK·callback·close를 기다리지 않고
bind terminal을 반환한다. 이후 `boundSessionReplaced(51)`을 이전 exact session에 one-way로 보내고,
`IZLinkSession.OnActorBindingReplacedAsync(...)` callback에서 application이 client 안내를 보낼 수 있게 한다.
Callback이 성공 또는 실패로 terminal이 되면 Framework가 non-blocking timer를 예약하고 turn을 즉시 반환한다.
Timer는 100 ms 뒤 connection을 닫으며 outbound queue가 먼저 비어도 시간을 줄이지 않고 sleep이나 serial
executor 점유로 기다리지 않는다. 통지·callback·close 실패는 새 binding을 rollback하지 않는다.

초기 gap의 ACK-before-swap 경로와 callback 부재를 제거했다. `IZLinkSession`에는 기존 구현체를 깨뜨리지 않는
default method를 추가했고(`Contracts/Streams/IZLinkSession.cs:17-30`), Actor owner는 새 binding을 publish·complete한
뒤 이전 owner에 대한 통지를 detached bounded retry로 제출한다(`Runtime/Host/ZLinkFrameworkRuntimeActors.cs:2727-2914`,
`Runtime/Host/ZLinkFrameworkRuntimeBoundSessionReplacement.cs:77-129`). Bind terminal은 이전 callback이나
cleanup ACK를 기다리지 않는다.

`boundSessionReplaced(51)` codec은 canonical record와 malformed/trailing input을 검증한다
(`Runtime/Service/ZLinkServiceBoundSessionReplacedWireCodec.cs:36-165`). Mesh transport는 Actor authority target
RID와 admitted source lifecycle을 검증하고, receiving node는 Actor authority fence로 local Actor를 찾지 않고
retired session owner의 Node RID·lifecycle generation·owner ID·owner lease generation·session RID·binding
generation·token을 exact-match한 경우에만 callback을 enqueue한다
(`Runtime/Service/ZLinkManagedMeshNode.cs:4424-4611`, `Runtime/Host/ZLinkFrameworkRuntimeBoundSessionReplacement.cs:7-49`).

Callback을 enqueue하기 전에 session을 closing으로 전이하고 신규 application dispatch를 거부한다
(`Runtime/Streams/ZLinkStreamSessionRuntime.cs:269-322`). Callback 안의 `Context.Client.Send(...)`는 허용하며
application이 `Context.CloseAsync()`를 호출하지 않아도 된다. Callback terminal 뒤에는 serial executor 밖의
100 ms timer를 예약하고, timer callback이 exact identity를 재검증한 뒤 Framework가 transport를 close한다
(`Runtime/Streams/ZLinkStreamSessionRuntime.cs:729-835`). 실제 callback deadline은 즉시 force-close하고, callback·전송·close
실패는 새 binding을 rollback하지 않는다. `Task.Delay`는 detached admission retry에만 존재하며 serial lane에서
100 ms를 기다리는 경로는 없다.

Actor-side state test는 publish 후 terminal, cleanup 불가 시 rollback 금지와 old bind replay 거부를 확인한다.
이전 session owner regression은 callback 전 closing, outbound 안내, fixed timer, callback failure와 deadline,
exact retired identity 재검증, duplicate, multi-Actor cleanup과 detached admission retry를 직접 실행한다
(`tests/Zlink.Framework.UnitTests/Runtime/BoundSessionReplacementLifecycleTests.cs:14-289`). Mesh receiver test는
admitted Actor-authority source RID·lifecycle과 receiving owner lifecycle이 다른 record를 production transport
경로에서 거부한다(`tests/Zlink.Framework.UnitTests/Runtime/ServiceRuntimeFoundationTests.cs:194-274`).
Worktree canonical candidate fixture, package/API snapshot과 세 process `ST-E2` happy path도 통과했다. historical
process evidence는 `framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-142226-2679994/`와
`framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-205326-1570824/`이며, 최신 current-source evidence는
`framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-212305-2229477/`이다. session-a의
`actor_binding_replaced`가 `disconnected`보다 먼저 기록되고 actor-b의 `success_reply` 뒤 `bound_push|after-rebind`가
기록된다. 최신 candidate의 full unit·package·process 재실행 결과와 canonical source integration은 아래 검증 표에서
별도로 추적하며, canonical source가 `main`에 들어오기 전에는 이 gap을 완료로 판정하지 않는다.

#### DOTNET-SESS-REPLACE-002 — `OperationCanceledException` failure를 deadline으로 오인

**판정: 완료**

공통 계약은 callback이 성공 또는 실패로 terminal이 되면 100 ms timer를 예약하고, lifecycle deadline 안에
terminal이 되지 않은 경우에만 deadline에서 즉시 close하도록 구분한다
(`framework/doc/framework/common/spec/20-session-actor-dispatch.ko.md:166-171`). Runtime은 linked callback token이 실제로
취소된 `OperationCanceledException`만 deadline으로 분류한다. Application이 token 취소 없이 던진 같은 예외는 일반
callback failure로 진단하고 fixed 100 ms timer를 예약한다
(`Runtime/Streams/ZLinkStreamSessionRuntime.cs:731-760`). `Callback_OperationCanceledException_Is_A_Failure_With_The_Fixed_Grace_Window`와
`Callback_Deadline_Force_Closes_Without_Starting_The_Grace_Timer`가 두 terminal을 deterministic `TimeProvider`로 분리한다.
정식 spec은 변경하지 않았다.

#### DOTNET-SESS-REPLACE-TEST-001 — 이전 session owner lifecycle regression 누락

**판정: 완료**

`BoundSessionReplacementLifecycleTests`는 command receiver 이후의 session lifecycle을 production
`ZLinkStreamSessionRuntime`과 실제 serial executor로 실행한다. 다음 owner-layer assertion을 고정했다.

- callback 전에 closing으로 전이하고 새 inbound application dispatch를 거부하는지 확인한다.
- callback의 `Context.Client.Send(...)`는 허용하고 callback 성공·일반 failure 뒤 100 ms 전에는 close하지 않는지 확인한다.
- 실제 deadline에서는 serial lane을 점유하지 않고 즉시 force-close하는지 확인한다.
- stale 또는 duplicate command가 callback이나 close를 반복하지 않고, same-current physical session이 self-notify하지 않는지 확인한다.
- 이전 physical session에 남은 여러 Actor binding을 각각 한 번 정리하면서 replacement binding은 제거하지 않는지 확인한다.
- notification admission retry가 bind terminal과 분리되고 retry 실패가 replacement를 rollback하지 않는지 확인한다.

`ServiceRuntimeFoundationTests.BoundSessionReplaced_Transport_Requires_The_Admitted_Authority_And_Retired_Owner_Lifecycles`는
codec-only test와 달리 두 `ZLinkManagedMeshNode`를 실제로 admit한 뒤 정상 record 한 건과 forged source RID,
source lifecycle, receiving owner lifecycle을 수신시킨다. 잘못된 record는 protocol error가 되고 callback handler
count가 증가하지 않는다. Exact owner ID·lease, session RID와 retired binding generation까지 같은 transport
경로에서 확인하기 위해 `BoundSessionReplacementLifecycleTests.Replacement_Transport_Reaches_Runtime_And_Requires_Exact_Owner_Fence`
를 추가했다. 이 test는 두 개의 실제 `ZLinkFrameworkRuntime`을 시작하고 source의
`IZLinkBackendBoundSessionReplacementNotifications`를 통해 record를 전송한다. Target의
`ZLinkSpotNodeRuntime`이 설치한 handler가 `TryHandleBoundSessionReplacedNotification`을 호출하므로 정상 record는
retired session callback terminal까지 도달하고, 네 필드를 각각 변조한 record는 callback과 close 없이 거부된다.
최신 replacement owner gate와 owner/lifecycle focused gate는 net8.0과 net10.0에서 각각 90/90으로 통과했고,
전체 unit 결과는 최신 검증 표에서 별도로 기록한다.

#### DOTNET-SESS-REPLACE-TEST-002 — exact owner fence의 transport-to-runtime regression

**판정: 완료**

`ZLinkManagedMeshNode.IsValidBoundSessionReplacedSource`는 source RID, source lifecycle generation, receiving
owner node RID·generation을 transport admission에서 확인한다(`Runtime/Service/ZLinkManagedMeshNode.cs:4592-4611`).
`ZLinkFrameworkRuntime.TryHandleBoundSessionReplacedNotification`은 이후 local owner ID·lease, retired session RID와
binding generation을 exact lookup에 사용한다(`Runtime/Host/ZLinkFrameworkRuntimeBoundSessionReplacement.cs:7-49`).
`BoundSessionReplacementLifecycleTests.Replacement_Transport_Reaches_Runtime_And_Requires_Exact_Owner_Fence`
는 두 개의 실제 `ZLinkFrameworkRuntime`을 net8/net10 runtime backend로 시작하고, TCP MeshNode admission 뒤
source의 `IZLinkBackendBoundSessionReplacementNotifications`를 통해 target에 record를 전송한다. Target의
`ZLinkSpotNodeRuntime`이 설치한 handler가 `TryHandleBoundSessionReplacedNotification`을 호출하므로 codec이나
notification counter만 검사하지 않는다. 정상 record는 retired session callback terminal까지 도달하고, 같은
transport 경로에서 다음 네 필드를 각각 변조한 record는 callback과 close 없이 거부된다.

- session owner ID
- session owner lease generation
- retired session RID
- retired binding generation

기존 source RID·source lifecycle·receiving owner lifecycle rejection도 함께 유지된다. 새 test와 기존 replacement
transport/lifecycle test를 합친 최신 owner-focused gate는 net8.0과 net10.0에서 각각 90/90으로 통과했고,
transport-to-runtime assertion도 이 gate에 포함된다. 이 항목은 완료로 올리지만 canonical command 51 source 통합 blocker는
`DOTNET-CONTRACT-INTEGRATION-001`에 별도로 남긴다.

#### DOTNET-CONTRACT-INTEGRATION-001 — command 51 canonical protocol source 미통합

**판정: BLOCKED (formal protocol 변경 승인 대기)**

현재 worktree의 schema·generator·validator에는 command 51이 있고, 기준 `137f2858bf`에는 없던 golden이 현재
`main` HEAD `cca88f92ac`에서 tracked 상태다. 그러나 현재 `main`의
`framework/runtime/protocol/service-wire-v1.schema.json`은 command 51을 정의하지 않고
`reservedCommandRanges`에서 `51..255`를 계속 예약한다. 반면 committed .NET generated constant와 runtime codec은
command 51을 사용하며, schema·Node/C++ generated asset·validator의 command 51 확장은 아직 현재 worktree의
dirty 변경이다. 따라서 golden이 tracked가 된 사실만으로 formal schema amendment가 완료된 것은 아니다.

따라서 현재 worktree에서 fixture validator가 `boundSessionReplaced=pass`를 출력해도 clean `main`의 canonical
protocol source로 재생성·검증할 수 없다. 공용 protocol 파일은 통합 담당자 소유이며 formal schema 변경은 사용자의
명시적 승인 없이는 수행하지 않는다. 승인된 canonical schema·golden·generator·validator가 함께 통합되고 clean
source에서 validator가 통과하기 전에는 이 항목과 `DOTNET-SESS-REPLACE-001`을 종결하지 않는다.

이 blocker가 요구하는 변경 범위는 다음으로 한정한다.

- `framework/runtime/protocol/service-wire-v1.schema.json:4741-4765`에 infrastructure command `51`,
  `actorAuthority`와 `retiredSession` route fence, no-flag/no-payload 규칙과 semantic constraint를
  canonical command로 등록한다.
- 같은 파일의 `reservedCommandRanges`를 `51..255`에서 `52..255`로 조정한다. 현재 dirty candidate에는
  이 diff가 있지만 기준 commit `137f2858bf7fd29f58405893473be8e773725a93`의 clean `main`에는 없다.
- `framework/runtime/protocol/golden/bound-session-replaced-v1.json:1-19`의 정상 record와 malformed/trailing
  bytes를 canonical fixture로 통합하고, generator·validator·generated asset을 schema에서 재생성한다.

이 변경은 .NET만의 내부 구현 변경이 아니라 모든 Framework 언어가 공유하는 wire contract 변경이다. 반대로
현재 runtime이 이미 따르는 “새 binding을 먼저 current로 확정하고 ACK를 기다리지 않음”, callback terminal 뒤
100 ms close 같은 동작을 이 blocker를 우회하기 위해 다른 public API나 raw frame으로 바꾸지 않는다. 사용자가
formal protocol 변경을 승인하거나 공용 통합 담당자가 해당 commit을 제공하기 전까지는 위 파일을 수정하지 않고,
typed identifier 잔여·exact owner transport regression·Windows/6 RID·process suite처럼 독립된 작업을 계속한다.

#### DOTNET-DOC-001 — exact interface 구현 차이 표가 현재 runtime과 불일치

**판정: BLOCKED (보호 문서 수정 승인 대기)**

보호 문서인 `.NET` exact interface의 구현 차이 표는 runtime에 command 51 송수신, callback과 non-blocking
100 ms close timer가 아직 없다고 기록한다
(`framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md:155-157`). 현재 runtime과
package에는 이 API와 경로가 있으므로 사실과 다르다. 이 문서는 보호 경로이며 이번 검토에서는 수정하지 않았다.
해당 행을 제거하거나 현재 남은 implementation gap으로 바꾸려면 보호 spec 수정 범위에 대한 명시적 승인이 필요하다.

#### DOTNET-DOC-002 — 공통 implementation gap의 .NET 행이 현재 runtime과 불일치

**판정: BLOCKED (보호 문서 수정 승인 대기)**

보호 문서 `framework/doc/framework/common/spec/90-implementation-gap.ko.md:18`은 .NET이 이전 cleanup을
기다린 뒤 route를 변경하고 command 51과 public callback이 없다고 기록한다. 현재 runtime은 새 binding을 먼저
current로 publish하고 detached one-way notification을 보내며 package public surface에도 default callback이 있다.
이 행은 현재 구현과 반대이지만 정식 spec 보호 경로이므로 수정하지 않았다. Exact interface와 함께 갱신하려면
두 파일의 구현 차이 행에 대한 명시적 수정 승인이 필요하다.

#### DOTNET-HTTP-SERIALIZATION-001 — public ActorRef·SpotRef HTTP JSON 경계

**판정: 완료 (스펙 변경 blocker 아님)**

초기 TA-A4 실패는 `framework-json-v1`의 기존 object-reference 계약을 public HTTP 경계에서 복원하지 못한
production 결함이었다. `nodeRid`는 정식 계약에서 문자열이어야 하며(`framework/doc/framework/common/spec/04-message-model.ko.md:70-92`),
caller의 `Results.Ok(actor)`와 `Zlink.HttpClient`의 typed `Async<ActorRef>()` 사이에서 raw 변환·DTO 우회·검증
완화는 허용하지 않는다. `ActorRef`와 `SpotRef`가 type-owned converter와 constructor/property invariant를 직접
소유하도록 보강했다(`framework/languages/dotnet/src/Zlink.Framework/Contracts/Actors/ActorRef.cs:1-186`,
`framework/languages/dotnet/src/Zlink.Framework/Contracts/Spots/Contracts.cs:1-157`). property 순서와 이름,
decimal-string generation, lowercase hex RID, duplicate/unknown/null/missing/범위 초과 값 거부를 동일 boundary에서
검증한다.

`FrameworkJsonProfileTests`의 exact round-trip·malformed vector는 22/22로 통과했고, 최신 net8/net10 전체
aggregate는 각각 1,622/1,622였다. 최신 package verifier는 9개 package·standalone HTTP clean consumer와
API snapshot `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를 통과했다. 최신 TA-A4도
`framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-212328-2239176/`에서 caller·Actor·Session
세 process의 ActorRef HTTP decode와 `to-actor-messaging e2e result=passed`를 확인했다. 이 항목은 기존 public
spec을 구현한 것이며 새 spec/protocol 변경을 요구하지 않는다. Config 14 전체와 나머지 process suite는
사용자 지시에 따라 deferred로 별도 추적한다.

#### DOTNET-WINDOWS-EVIDENCE-001 — regression matrix의 Windows runner evidence 미실행

**판정: 미실행**

`framework/doc/framework/dotnet/internals/regression-test-matrix.ko.md:64-70`은 MeshNode socket option,
ShoppingMall, GameQuest, TicTacToe와 ShoppingMall workflow gap의 남은 evidence를 모두 Windows PowerShell
runner로 기록한다. 현재 환경에는 `pwsh`가 있지만 Linux에서 PowerShell script를 실행하는 것은 Windows socket,
path와 process 동작을 검증하지 않으므로 이 evidence를 대체하지 않는다. Windows runner 결과가 없으면 최종
Codex Sol 관문의 “미실행 필수 gate 0” 조건을 충족하지 않는다.

#### DOTNET-E2E-INSTANCE-001 — Config 14 Instance Spot process 분모 미완료

**판정: 미완료**

사용자가 이번 작업에서는 E2E를 추가 실행하지 않고 후속 작업에서 재개하도록 지시했으므로, 아래 feature-map과
runner 상태는 `deferred` gate로 유지한다. 이미 통과한 개별 process evidence를 Config 14 전체 완료로 승격하지 않는다.

`framework/languages/dotnet/e2e/InstanceSpot/feature-map.ko.md:16-48`에서 검증 완료인 항목은
IS-E2E-01~03, 05, 08, 35이고 IS-E2E-06은 부분 구현이다. 나머지 시나리오는 미구현으로 남아 있다.
Selector runner도 이 subset만 위임하며 default `all`과 미구현 selector는 exit 2로 종료한다
(`framework/languages/dotnet/e2e/InstanceSpot/run_e2e.sh:13-39`). Feature-map inventory test와 일부
process 통과는 36개 시나리오의 실제 process evidence를 대신하지 않는다. Config 14 전체 runner가 성공하고
각 scenario의 client-visible 결과와 role evidence가 남기 전에는 전체 .NET Framework gap을 완료로 판정할 수 없다.
현재 candidate에서 `bash framework/languages/dotnet/e2e/InstanceSpot/run_e2e.sh all`도 같은 안내와 함께
exit 2를 재확인했다. 따라서 이 항목은 단순 문서 누락이 아니라 현재 runner 분모가 실제로 미완료인 상태다.

#### DOTNET-E2E-PROCESS-001 — 전체 process Gap·PARTIAL 분모 미완료

**판정: 미완료**

전체 process suite 재실행도 같은 지시에 따라 후속 작업으로 보류한다. 따라서 이 항목은 현재 source의
미구현·PARTIAL 목록과 미실행 process evidence를 보존하며 완료로 판정하지 않는다.

최종 Codex Sol 반복 review에서 Config 14만 별도 기록한 것은 전체 process inventory를 포괄하지 못한다는
누락을 확인했다. 다음 suite에도 `미구현`, `부분 구현`, `diagnostic only` 또는 `actual-process 미검증`
항목이 남아 있다.

- SpotActorTransfer는 relocation binding·owner pause·barrier·대규모 inventory와 성능 profile을 포함한
  다수 항목이 미구현 또는 diagnostic only다
  (`framework/languages/dotnet/e2e/SpotActorTransfer/feature-map.ko.md:19-100`).
- ResilienceLifecycle은 preflight capacity 경쟁, Actor owner ABA fence, 언어 간 terminal 해석과
  activated seal 경계 증거가 없다
  (`framework/languages/dotnet/e2e/ResilienceLifecycle/feature-map.ko.md:38-51`).
- ChannelEgressRouting은 drain, deadline·disconnect race와 lifecycle generation을 포함한 일부 항목이
  partial 또는 실행 대기다
  (`framework/languages/dotnet/e2e/ChannelEgressRouting/feature-map.ko.md:27-37`).
- LocationMessaging의 `RM-A7` global ID 충돌 시나리오는 미구현이다
  (`framework/languages/dotnet/e2e/LocationMessaging/feature-map.ko.md:12`).
- PubSub automatic topology·status·failure 시나리오는 source-only 또는 미구현이다
  (`framework/languages/dotnet/e2e/PubSub/feature-map.ko.md:21-37`).
- SubmitAdmission은 대부분의 family가 부분 구현 또는 미구현이며 aggregate 회귀에도 남은 반복 분모가 있다
  (`framework/languages/dotnet/e2e/SubmitAdmission/feature-map.ko.md:13-36`).
- StoreFailure의 stateful fence·reconcile·interop·relocation recovery·capacity 시나리오는 미구현이다
  (`framework/languages/dotnet/e2e/StoreFailure/feature-map.ko.md:24-42`).

Regression matrix의 inventory test는 scenario ID와 feature-map의 대응을 확인할 뿐 각 scenario가 실제
process에서 종결됐음을 증명하지 않는다
(`framework/doc/framework/dotnet/internals/regression-test-matrix.ko.md:494`). 위 Gap·PARTIAL을 구현하고
각 runner의 client-visible 결과와 역할별 lifecycle evidence를 확보하기 전에는 모든 Framework Gap을
검토·완료했다고 판정하지 않는다.

#### DOTNET-SESS-REPLACE-FRESHNESS-001 — current replacement process evidence

**판정: 완료**

이전 identifier residual candidate의 `SpotActorTransfer ST-E2` evidence
`logs/20260808-172823-4022771/`와 `logs/20260808-182644-2031944/`는 historical로 보존한다. 최신 typed
owner-key source에서 다시 실행한 current evidence는 `logs/20260808-212305-2229477/`이며, Actor owner·이전
session owner·새 session owner 세 process와 callback guidance-before-close를 확인했다. 동일 Sol 최종 재검토에서도
replacement freshness 종결을 확인했다. TA-A1은 기본 Actor request/reply 경로만 보강하므로 replacement evidence를
대체하지 않는다.

#### DOTNET-SUBMITTER-DISPOSE-RACE-001 — send-ready와 Dispose의 lifecycle 경쟁

**판정: 완료**

current identifier candidate의 첫 net10.0 aggregate에서
`ZLinkAsyncSubmitterTests.DisposeAsync_Twice_RacingReady_CleansPendingResourcesOnce`가
`/tmp/zlink-dotnet-unit-net10-20260808-after-identifier-residual.log:12-26`에서 실패했다.
  `ZLinkAsyncSubmitter.OnSendReady`가 `_accepting`을 확인한 뒤 `DisposeAsync`가 native submitter를 정리하면
`TrySubmitNow`의 `ThrowIfDisposed`가 callback task 밖으로 `ObjectDisposedException`을 전파할 수 있다
(`Runtime/Messaging/ZLinkAsyncSubmitter.cs:456-465,603-670`,
  `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/ZLinkAsyncSubmitterTests.cs:579-586`). production이
ready callback의 dispose race를 pending terminal로 변환하도록 수정했고, net8/net10 focused와 전체 aggregate가
각각 통과했다(`/tmp/zlink-dotnet-unit-net8-20260808-after-fanout-submit-race.log`,
`/tmp/zlink-dotnet-unit-net10-20260808-after-fanout-submit-race.log`). 동일 Sol 재검토에서 이 lifecycle
불변 조건의 종결을 확인했으며, timeout 완화나 test retry로 실패를 숨기지 않았다.

#### DOTNET-RELEASE-MATRIX-001 — net10.0과 6개 RID release gate 부분 실행

**판정: 부분 실행**

`framework/doc/framework/dotnet/internals/regression-test-matrix.ko.md:267-275`는 unit·single-process·
multi-process 전체, `net8.0`·`net10.0`, 6개 runtime RID CI를 release gate로 고정한다. 별도 `/tmp/zlink-dotnet10`
SDK `10.0.302`로 `ZLinkFrameworkTargetFrameworks=net10.0`, Linux x64, Release unit aggregate를 실행해
1,622/1,622를 통과했다. 현재 실행한 것은 Linux x64의 net8.0·net10.0 Release unit aggregate뿐이다. 이 실행에서
bytes-only relocation listener 경로가 드러나 `ZLinkRuntimeMetrics.CreateRelocation`이 `RelocationBytes.Enabled`도
고려하도록 수정했고, net10 analyzer 경고 2건도 제거했다
(`Runtime/Diagnostics/ZLinkRuntimeMetrics.cs:250-260`, `Runtime/Host/ZLinkFrameworkRuntimeLocationAccess.cs:27-33,71-82`).
양쪽 TFM의 single/multi-process와 Linux x64를 포함한 6개 RID 전체의 실제 full runtime gate는 실행하지 않았다.
Package가 6개 RID native asset을 포함하는지 검사하는 것과 각 RID에서 Framework gate를 실행하는 것은 다른
증거다. Public contract test는 net8.0과 net10.0에서 각각 76/76으로
통과했고, `PublicContractSnapshot`은 net10의 `NullabilityInfo`가 생성자 unannotated generic parameter를
다르게 투영하는 경우에도 net8 기준 snapshot을 유지하도록 보정했다
(`framework/languages/dotnet/scripts/PublicContractSnapshot.cs:151-190,237-330`). 이는 contract snapshot
renderer의 호환성 보정이며 public API나 formal spec을 변경하지 않는다.

#### DOTNET-UNIT-AGGREGATE-001 — aggregate 실행의 transient timeout

**판정: 완료**

`_spots` typed-key 변경 직후 aggregate에서 서로 다른 timing/resource-sensitive test가 1,603/1,605와
1,604/1,605로 실패했다. Isolated 재실행은 각각 통과했으며, 원인은 test가 aggregate의 native context
startup·teardown 지연을 5초 timeout으로 취급한 데 있었다. production runtime과 계약은 바꾸지 않고
`StatefulServiceRuntimeTests.WaitUntilAsync`의 bounded wait를 30초로 늘리고
`RouteMeshRuntimeServiceTests`의 observer/status budget과 `DrainCoordinatorTests`의 orderly close budget을
30초로 맞췄다(`tests/Zlink.Framework.UnitTests/Runtime/StatefulServiceRuntimeTests.cs:3783`,
`RouteMeshRuntimeServiceTests.cs:440-524`, `DrainCoordinatorTests.cs:1234-1243`). 최신 typed owner candidate의
Linux x64 Release aggregate는 net8.0과 net10.0에서 각각 1,622/1,622로 통과했고 owner/lifecycle
focused test는 각 TFM에서 235/235, replacement owner filter는 20/20으로 통과했다. Negative configuration test가 의도된 Host startup rejection을
로그로 남기는 것은 기존 동작이다.

#### DOTNET-REGRESSION-DOC-001 — regression matrix의 현재 gate 설명이 source와 충돌

**판정: 완료**

Regression matrix의 RouteMesh 항목을 Framework-level `MaxMessageSize` 없이 directional HWM·mailbox budget·
send/receive timeout만 확인하도록 갱신했다
(`framework/doc/framework/dotnet/internals/regression-test-matrix.ko.md:107`). 이는 RouteMesh ServerServer에
complete-message size cap을 두지 않는 정식 판단과 current runtime에 맞는다. Config 12 aggregate row도
`all` selector가 현재 26개 scenario를 순서대로 실행하고 성공 시 exit 0으로 끝나는 동작으로 갱신했다
(`framework/doc/framework/dotnet/internals/regression-test-matrix.ko.md:497`,
`framework/languages/dotnet/e2e/ChannelEgressRouting/run_e2e.sh:13-27`). 개별 feature-map의 partial scenario는
process Gap 분모로 별도 유지하므로 이 문서 정정이 Config 12 전체 process 완료를 의미하지 않는다.

#### DOTNET-POSDDD-REVIEW-001 — 2차 runtime/test 구조 review 대기

**판정: 대기**

사용자가 정한 순서에 따라 모든 계약·구현 항목이 1차 Codex Sol review에서 CLEAN이 된 뒤에만 production
runtime POSD/DDD review를 시작한다. 현재는 canonical source와 보호 문서, Windows evidence가 남아 있으므로
2차 review를 시작하지 않았다. 1차 pre-review는 detached replacement retry가 최대 30초 동안 10 ms마다
record를 다시 encode하고 retry diagnostic 문자열을 만드는 Medium 성능 후보를 찾았다
(`Runtime/Host/ZLinkFrameworkRuntimeBoundSessionReplacement.cs:93-119`,
`Runtime/Service/ZLinkManagedMeshNode.cs:604-627`). 이 항목은 계약 관문을 우회해 먼저 리팩터링하지 않고,
2차 review가 시작되면 production runtime의 첫 finding으로 재검증한다.

## 3. 반증 검토와 제외한 후보

각 발견을 “코드 모양이 다르다”는 이유만으로 gap 처리하지 않고, 반대 근거가 있는 후보는 제외했다.

- ClientServer update에 `MaintenanceWave`·placement capacity가 없다는 사실은 gap으로 세지 않았다. Canonical `client-server-admission` schema 자체가 server descriptor를 channel, RID/generation/revision, weight/state, identity, message bound와 endpoint로 닫고 있기 때문이다(`framework/runtime/protocol/service-wire-v1.schema.json:2888-2922`). Internals의 generic mutable-field 문장만 떼어 schema보다 넓게 적용하지 않았다.
- StreamNode의 외부 STREAM C→S runtime 의미는 64 KiB 단방향 계약과 일치한다. Registration은 전용 64 KiB 기본값을 사용하고(`Runtime/Configuration/ZLinkFrameworkRegistration.cs:295-315`), receive buffer가 prefix를 제외한 header+payload만 검사하며(`Runtime/Streams/ZLinkStreamReceiveBuffer.cs:34-52`), server outbound writer에는 이 설정의 크기 검사가 없다. 다만 public 설정 표면은 DOTNET-API-003으로 분리한 fluent API gap이므로 전체 종결로 판정하지 않는다. 이 계약은 ClientServer Channel이나 RouteMesh SS에 적용하지 않는다.
- Message Follow는 type/handler 존재만 본 것이 아니라 source peer lifecycle fence, object identity/generation, hop/count/byte bounds와 cache invalidation fence test가 있는 경로를 확인했다. 이번 재검토에서 그 종결 판정을 뒤집을 반례는 찾지 못했다.

## 4. 이번 검토에서 source/unit 범위 종결을 확인한 과거 후보

과거 gap 기록을 그대로 재사용하지 않고 현재 source에서 다시 확인했다.

- RouteMesh inbound HWM: process와 owner 회계를 분리하는 결정(`common/internals/08-object-lifecycle.ko.md:219-268`)에 대해 managed node/pump가 `ZLinkInboundDispatchBudget` lease를 유지하고(`Runtime/Service/ZLinkManagedMeshNode.cs:3700-3760`, `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:385-480`), RouteMesh mailbox pause/resume assertion이 있다(`UnitTests/Runtime/StatefulServiceRuntimeTests.cs:471-590`).
- Weighted selection: smooth weighted round-robin과 stable RID tie-break 결정(`common/internals/06-routing-and-cache.ko.md:203-255`)을 공통 selection plan이 구현하고(`Runtime/Channels/ZLinkWeightedSelector.cs`, `Runtime/Channels/ZLinkWeightedSelectionPlan.cs`), exact order·tie-break·candidate replacement assertion이 있다(`UnitTests/Runtime/WeightContractTests.cs:61-230`).
- Instance Spot idle eviction: bounded scan 결정에 대해 catalog가 64개 batch와 cursor wrap을 사용하고(`Runtime/Spots/ZLinkSpotNodeCatalog.cs:29,580-638`), 연속 batch가 뒤 후보까지 도달하는 assertion이 있다(`UnitTests/Runtime/StatefulServiceRuntimeTests.cs:2402-2455`).
- Observer queue: intermediate coalescing과 bounded terminal loss 공개 결정에 대해 별도 terminal queue와 loss count를 구현하고(`Runtime/Diagnostics/ZLinkObservationQueue.cs`), terminal 보존·oldest discard·subscriber loss assertion이 있다(`UnitTests/Runtime/ZLinkObservationQueueTests.cs:6-85`).
- Cross-owner session rebind의 tombstone/publish 분리와 ACK-before-swap assertion은 이전 계약에 대한
  과거 증거다. 현재는 `DOTNET-SESS-REPLACE-001`의 즉시 bind·one-way 교체 통지·exact owner callback 경로로
  대체되었고, 이전 binding의 일반 tombstone cleanup은 새 current binding과 분리되어 유지된다.
- Message Follow: route fence와 cache invalidation 결정(`common/internals/12-service-wire-protocol.ko.md:185-210`)에 대해 admitted-peer handler와 exact route invalidation이 있고(`Runtime/Service/ZLinkManagedMeshNode.cs:543-565`, `Runtime/Locations/ZLinkStoreLocationResolvers.cs:294-350`), generation fence·matching cache-only invalidation assertion이 있다(`UnitTests/Runtime/ActorHandoffTests.cs:627-680`, `UnitTests/Runtime/LocationResolverTests.cs:90-125`).

이 항목들은 source와 표시한 unit assertion 범위에서 해당 결정과 일치한다는 판정이다. 실제 multi-process E2E 전체 통과나 package/clean-consumer 종결을 뜻하지 않는다.

## 5. 종결 검증 결과

| 검증 | 결과 | 해석 |
|---|---|---|
| `.NET` contract/API surface | 통과: API snapshot 및 package contract | `IZLinkSession.OnActorBindingReplacedAsync(...)` default interface method를 포함한 public API snapshot이 package와 일치한다 |
| Framework 전체 unit test | current typed owner-key candidate 통과: net8.0 1,622/1,622, net10.0 1,622/1,622 | callback failure/deadline, fixed timer, exact retired owner, multi-Actor cleanup, detached retry와 production transport source/lifecycle 및 exact owner fence regression을 포함한다 (`/tmp/zlink-dotnet-unit-net8-20260808-typed-owner-residual.log`, `/tmp/zlink-dotnet-unit-net10-20260808-typed-owner-residual.log`) |
| packaged-contract/clean consumer | current typed owner-key candidate 통과 | strict `verify_packaged_contract.sh`가 9개 package와 standalone HTTP clean consumer를 통과했고 public API snapshot hash는 `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`이다. Framework XML contract hash는 `19288e149fadd087791bbc38f730d7a906fd3d66ac3c36f7aef50207d872c42b`로 고정했다 (`/tmp/zlink-dotnet-package-typed-owner-residual-strict.log`) |
| service wire 3개 gate | worktree 통과, `main` 미통합 | current dirty schema 기준 `validate-service-wire-schema.mjs`는 42 commands, `generate-service-wire-assets.mjs --check`는 commands=42/fixtures=37, `verify-service-wire-decoder-fixtures.mjs`는 `boundSessionReplaced=pass`로 끝났다 (`/tmp/zlink-wire-validate-doc-refresh.log`, `/tmp/zlink-wire-generate-doc-refresh.log`, `/tmp/zlink-wire-fixtures-doc-refresh.log`). `main` schema는 command 51을 reserved로 둔다 |
| 관련 process E2E | current typed owner-key source에서 TA-A4와 replacement ST-E2 모두 통과 | `ToActorMessaging/logs/20260808-212328-2239176/`에서 caller·Actor·Session 세 process의 ActorRef HTTP decode를 통과했고, `SpotActorTransfer/logs/20260808-212305-2229477/`에서 Actor owner·이전 session owner·새 session owner와 callback guidance-before-close를 확인했다. Failure·duplicate·stale 분기는 unit owner regression이 검증한다. Config 14 전체와 나머지 process suite는 사용자 지시에 따라 deferred다 |

이 표는 `.NET` gap 종결 gate를 구분해 기록한다. 저장소 전체 언어의 aggregate contract와 전체 process
suite를 실행했다는 뜻으로 확대하지 않는다.

직전 `_spots` typed-key candidate에서 aggregate 실행이 각각 1,603/1,605와 1,604/1,605로 끝난 기록은
historical checkpoint다. 관련 wait budget을 30초로 보정한 해당 candidate의 1,605/1,605 기록도 historical로
보존한다. 최신 typed owner-key candidate의 authoritative 수치는 net8.0·net10.0 각각 1,622/1,622이며,
owner focused 수치는 각각 90/90이다. Negative configuration test가 의도된 Host startup rejection을 로그로
남기는 것은 기존과 같다.
이 plan과 이번 .NET 변경 파일의 path-limited `git diff --check -- <path>`는 통과했다. 다른 언어의 기존
dirty 변경에는 trailing whitespace가 남아 있어 전역 `git diff --check`는 통과했다고 기록하지 않는다.
이전 full run에서 발견한 replacement timer test race는 fake time을 진행하기 전에 scheduler timer 등록을
기다리도록 수정했다. 최신 replacement owner filter와 전체 suite 재실행에서 같은 race는 재현되지 않았다.

### 2026-08-08 source·package·process 재검토 (historical checkpoints)

Session 교체 계약과 기존 보고서 항목이 현재 source에 반영되었는지 다시 대조했다. RouteMesh
registration의 stale `MaxMessageSize` 사용은 `RuntimeMonitoring/Server/Service/ServiceHostFactory.cs`에서
제거했고, runtime state registry는 channel·SpotNode·StreamNode별 value type key로, Spot executor lane은
Actor·timer value type key로 바꿨다. 문자열로 들어오는 기존 호출은 typed dictionary boundary extension에서
검증·변환하고, registry와 lane 내부에는 value type만 보관한다.

Source와 package gate는 `Zlink.Framework.csproj` 및 UnitTests project build 0 warning/0 error, Framework unit
최신 aggregate 1,605/1,605와 replacement·timeout focused 3/3, `verify_packaged_contract.sh` 9개 package와 standalone HTTP
clean consumer, public API snapshot
`d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`로 다시 확인했다.
Service-wire schema validator, generated asset `--check`와 decoder fixture validator도 현재 worktree에서는
각각 통과했지만 command 51 canonical source가 `main`에 통합되지 않았으므로 종결 gate에서 제외한다.
대표 process는 `ToActorMessaging/TA-A1` (`logs/20260808-052229-1491148/`), `SpotService/sm-e4`
(`logs/20260808-052245-1505062/`), `ChannelEgressRouting/CH-E2E-01`
(`logs/20260808-052325-1556094/`), `ChannelEgressRouting/CH-REG-06`
(`logs/20260808-052531-1640268/`), `RegistrationCodec/RC-B1`
(`logs/20260808-052337-1566280/`), `InstanceSpot/IS-E2E-05`
(`logs/20260808-052423-1596039/`), `SpotActorTransfer/ST-A1,ST-F1`
(`logs/20260808-052456-1598222/`), `ResilienceLifecycle/RL-B4`
(`logs/20260808-052515-1633329/`)가 통과했다.

Session replacement process는 `SpotActorTransfer/ST-E2`를 최신 typed owner candidate의
`logs/20260808-142226-2679994/`에서 통과했다. Actor owner의 `success_reply`와 `bound_push|after-rebind`,
이전 session owner의 `actor_binding_replaced`와 이후 `disconnected`, 새 session owner의 bind evidence를
확인했으며 client는 callback guidance가 close보다 먼저 도착했음을 자체 assertion으로 확인했다.

초기 `LocationMessaging` 실행에서 보인 RM-A1·C1·C8 timeout은 provider E2E host의 4-byte native receive
budget 때문에 발생했다. `Server/Provider/ProviderHostFactory.cs`의 budget을 4 MiB로 조정한 뒤
`LocationMessaging` 15개 scenario 전체가 `logs/20260808-055401-2923930/`에서 통과했고, RM-C9는
Framework application HWM 1 MiB의 pause/resume와 후속 request까지 확인했다. 이 변경은 E2E 실행 조건만
보정했으며 formal spec과 공통 wire schema는 수정하지 않았다.

이전 identifier key refactor candidate에서 unit·process를 동시에 실행한 한 번의 run은 resource contention으로
unit 2건 timeout과 ST-E2 callback delivery failure를 기록했지만, 해당 unit 2건을 단독 재실행해 2/2 통과했고
ST-E2도 단독 재실행에서 통과했다. `_spots` typed-key 변경 뒤 aggregate는 1,603/1,605와 1,604/1,605로
반복해서 non-green이었지만 wait budget 보정 뒤 해당 candidate의 full aggregate는 1,605/1,605로 통과했다. 해당
candidate의 ST-E2와 package verifier는 각각 `logs/20260808-105021-214036/`, 9개 package, standalone HTTP clean consumer와
API snapshot을 통과했다.
이후 session binding key까지 포함한 직전 candidate의 재실행 결과는 아래 `typed binding-key candidate freshness gate`에
별도로 기록한다. 이 구분은 이전 binary 증거를 새 production source의 증거로 재사용하지 않기 위한 것이다.
이전 candidate에서도 `framework/languages/dotnet/scripts/verify_packaged_contract.sh`를 다시 실행해
9개 NuGet package, standalone HTTP clean consumer와 동일한 public API snapshot
`d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를 통과했다.

### 2026-08-08 Codex Sol 1차 계약 review 기록

- **대상과 reviewer**: `.NET` plan의 모든 Gap·PARTIAL·public contract, exact interface, 정식 spec,
  production runtime, owner-layer regression, package·clean-consumer와 process evidence를 같은
  `gpt-5.6-sol`, effort `high` reviewer가 검토했다.
- **기준**: commit `137f2858bf7fd29f58405893473be8e773725a93`와 그 위의 dirty candidate다. 다른 언어와
  보호 spec의 기존 dirty 변경을 포함하므로 수정 commit은 만들지 않았고 push하지 않았다.
- **첫 반복 결과**: `NOT CLEAN`, 독립 finding High 4건·Medium 6건이었다. 새 finding은 same-session
  idempotence에서 session owner node lifecycle generation을 비교하지 않는 문제와 replacement closing flag와
  application queue admission 사이의 race였다
  (`Runtime/Actors/ZLinkActorRuntimeState.cs:1819-1841`,
  `Runtime/Streams/ZLinkStreamSessionRuntime.cs:271-322`).
- **수정**: same-session 판정에 `SessionOwnerNodeGeneration`을 포함하고 generation mismatch regression을
  추가했다(`Runtime/Host/ZLinkFrameworkRuntimeActors.cs:2850-2859`,
  `tests/Zlink.Framework.UnitTests/Runtime/ActorBoundSessionRelayTests.cs:390-451`). 공통 serial queue에
  application-only admission close를 두고 replacement가 callback을 enqueue하기 전에 같은 admission lock에서
  닫도록 했다(`Runtime/Execution/ZLinkSerialExecutionQueue.cs:163-166,220-245`,
  `Runtime/Streams/ZLinkStreamSessionRuntime.cs:291-323`). Lifecycle lane은 유지하고 64개 concurrent application
  admission이 모두 `Closed`인지 검증했다.
- **재검토 결과**: 두 source finding은 종결됐다. Owner-focused 22건은 5회 연속 통과했다. 직전
  candidate의 전체 unit 1,605건 통과 기록이 있었지만, reviewer 당시 `_spots` typed-key 변경 뒤 aggregate는
  1,603/1,605로 두 timing/resource-sensitive test가 실패했다. 해당 두 test의 isolated 재실행은 2/2였지만
  aggregate gate를 대체하지 않으므로 필수 gate는 미통과로 유지한다. 같은 reviewer의 첫 반복 판정은
  `NOT CLEAN`, 독립 잔여 High 3건·Medium 5건이었다.
  Process feature-map 전체와 최종 evidence 기록을 다시 읽은 다음 반복에서는 누락된 process Gap·PARTIAL과
  evidence 불일치를 추가로 찾아 `NOT CLEAN`, 독립 잔여 High 4건·Medium 5건으로 판정했다. 최신
  aggregate 실패 gate는 wait budget 보정 뒤 1,605/1,605로 종결되었고 regression matrix stale 항목도
  현재 source에 맞춰 수정했으므로 현재 필수 미통과 finding은 Medium 4건이며, 요약 표의
  `DOTNET-SESS-REPLACE-001`은 여러 blocker를 묶는 상위 항목이고 `DOTNET-POSDDD-REVIEW-001`은 계약 review가
  CLEAN일 때만 시작하는 후속 관문이므로 이 독립 finding 수에는 다시 세지 않는다.
- **잔여 High**: command 51 canonical source의 clean `main` 통합, Config 14 Instance Spot 전체 process 분모,
  Instance Spot 외 7개 suite의 process Gap·PARTIAL, `net10.0`과 6개 RID release matrix다.
- **최종 기록 검토**: plan evidence 불일치는 focused count·wire 3개 gate·finding 산정 기준,
  `DOTNET-API-001`의 baseline/current 구분과 process feature-map 범위를 바로잡아 종결했다. 최신 aggregate
  실패는 isolated pass만으로 닫을 수 없는 별도 필수 gate finding으로 추가했다. 최종 독립 잔여는
  High 4건·Medium 4건이며, 별도 Low 1건은 `_spots` 선언을 stale하게 적은 근거 문장으로 확인 후 수정했다.
  따라서 현재 잔여 Low는 0건이고, 해당 Low 1건은 historical consistency finding으로만 기록한다.
- **잔여 Medium**: `DOTNET-LAYER-003`의 남은 string identity, 보호 문서 두 곳의 stale 구현 상태와 Windows
  PowerShell evidence다.
- **identifier 후속 수정**: Actor ownership·session gate·Spot membership·ordered relay·bound-session의
  dictionary key를 typed identifier로 전환하고 production build 0 warning/0 error, owner/session+identifier
  focused 53/53을 재실행했다. 같은 Sol reviewer는 method parameter와 추가 owner registry에
  남은 `string`을 확인해 `DOTNET-LAYER-003`을 계속 PARTIAL로 유지했다. 이 수정은 public contract나 formal
  spec을 변경하지 않았다.
- **재실행 gate**: 이번 read-only Sol 재검토 뒤 unit aggregate를 재실행해 1,605/1,605, 관련 focused test
  3/3을 통과했다. 이 문서와 이번 .NET 변경 파일에는 `git diff --check -- <path>`가 통과했다. 전체
  worktree에는 다른 언어의 기존 dirty 변경에서 trailing whitespace가 남아 있어 전역 `git diff --check`를
  통과했다고 기록하지 않는다. 이 gate는 종결했지만 canonical command 51, process, Windows와 release
  matrix gate는 여전히 열려 있다.
- **최신 문서 consistency review**: 같은 `gpt-5.6-sol`, effort `high`가 최신 plan과 source/evidence를 다시
  대조했다. 새 High/Medium은 없었고, `_spots` 근거 수정은 source와 일치한다. 다만 typed-key 세 registry의
  line과 focused count를 갱신하기 전에는 Low consistency finding 1건이 남아 있었다. 해당 line과 count를
  바로잡은 뒤 이 plan 및 이번 .NET 변경 파일의 path-limited `git diff --check`를 다시 통과했다. 현재
  잔여 집계는 High 4건·Medium 4건·Low 0건이며, 계약 단계는 `NOT CLEAN`이다.
- **최종 Sol 재확인**: 같은 `gpt-5.6-sol`, effort `high`가 line/count 수정 뒤 plan consistency를 다시 확인해
  `CLEAN`으로 판정했고 새 finding은 없었다. 계약 판정은 `NOT CLEAN`으로 유지되며 잔여는 High 4건·Medium
  4건·Low 0건이다. 당시에는 Windows PowerShell evidence와 `net10.0`·6 RID release matrix를 실행하지
  않았고,
  command 51 clean-main 통합과 Config 14·나머지 7개 process suite도 미완료다.
- **후속 net10 gate**: 별도 .NET SDK `10.0.302`의 Linux x64 Release unit aggregate를 실행해
  `net10.0` 1,605/1,605를 통과했다. 이 과정에서 발견한 bytes-only relocation metric 조건과 net10 analyzer
  warning을 수정했으며(`Runtime/Diagnostics/ZLinkRuntimeMetrics.cs:250-260`,
  `Runtime/Host/ZLinkFrameworkRuntimeLocationAccess.cs:27-33,71-82`), net8.0 Release aggregate도 다시
  1,605/1,605를 통과했다. 양쪽 TFM의 single/multi-process와 Linux x64를 포함한 6개 RID full gate는
  여전히 미실행이므로 release matrix를 완료로 올리지 않는다.
- **release evidence 표현 후속 검토**: 같은 Sol 재검토에서 “나머지 5개 RID”라는 표현이 Linux x64의
  full single/multi-process와 six-RID CI까지 끝난 것으로 축소 해석될 수 있다는 Medium finding을 확인했다.
  요약과 상세 판정을 Linux x64 net8/net10 unit만 실행했고 양쪽 TFM single/multi-process와 Linux x64를
  포함한 6개 RID 전체 full CI가 미실행이라고 바로잡았다(`dotnet-internals-spec-gap-report.ko.md:137,893-898`).
- **release evidence 표현 수정 후 최종 Sol 재확인**: 같은 `gpt-5.6-sol`, effort `high`가 수정된 요약·상세·
  후속 gate 기록과 source를 다시 대조해 문서 consistency를 `CLEAN`으로 판정했고 새 finding은 없었다.
  계약 단계는 `NOT CLEAN`, 잔여 집계는 High 4건·Medium 4건·Low 0건으로 유지된다. 필수 release 잔여는
  양쪽 TFM single/multi-process와 Linux x64를 포함한 6개 RID full CI, 그리고 Windows PowerShell evidence다.
- **contract renderer 후속 gate**: net10 contract test에서 `NullabilityInfo`가 생성자 generic parameter를
  net8과 다르게 투영하는 차이를 발견했다. 생성자 parameter에 직접 nullable annotation이 없는 경우에만
  generic parameter의 nullability를 net8 snapshot 기준으로 정규화하고, 명시적 nullable annotation이 있는
  method parameter와 property는 그대로 유지하도록 `PublicContractSnapshot`을 수정했다
  (`framework/languages/dotnet/scripts/PublicContractSnapshot.cs:151-190,237-330,351-354`). 수정 뒤
  net10.0 contract 76/76과 net8.0 contract 76/76, packaged verifier 9개 package·standalone HTTP
  clean consumer·API hash `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를
  재실행해 통과했다. 이는 Linux x64 unit·contract 증거를 보강하지만 6개 RID 전체와 process release gate를 대체하지 않는다.
- **직전 typed binding-key candidate freshness gate**: `ZLinkSessionBindingKey.ActorId`를 `ZLinkActorId`로 저장하고
  `FromBoundary`에서만 변환한 뒤, 같은 candidate에서 net8.0 Release unit 1,605/1,605, net10.0 Release unit
  1,605/1,605, net8.0·net10.0 contract 각각 76/76, `verify_packaged_contract.sh` 9개 package와 standalone
  HTTP clean consumer, owner/session·replacement focused test 61/61 (net8.0·net10.0 각각), 그리고 세 process ST-E2를
  다시 실행했다. 당시 ST-E2 log는
  `framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-121719-3248588/`이고 callback guidance가
  close보다 먼저 도착했다. 이 재실행으로 이전 Sol의 evidence-freshness Medium finding은 해소했지만 canonical
  command 51, Windows·6 RID release matrix, Config 14와 나머지 process suite, LAYER-003 residual은 여전히 남아 있다.
- **직전 typed binding-key 후속 Sol 재검토**: 같은 `gpt-5.6-sol`, effort `high`가 당시 plan·source·evidence를 다시
  대조해 consistency를 `CLEAN`으로 판정했고 새 finding은 없었다. 당시 net8.0·net10.0 focused 61/61, 당시 ST-E2
  callback guidance-before-close와 scenario pass, `Bind` source line 193을 확인했다. 전체 계약 단계는
  `NOT CLEAN`, 잔여는 High 4건·Medium 4건·Low 0건이다. 따라서 POSDDD 2차 review는 시작하지 않았다.
- **직전 typed route/identity candidate freshness gate**: session binding route와 confirmed identity의
  `MeshName`을 `ZLinkMeshName`으로 보관하고 core binding table `Bind`의 Actor ID를 `ZLinkActorId`로 고정한
  candidate를 별도로 검증했다(`Runtime/Actors/ZLinkSessionActorBindingTable.cs:27-120,161-218`,
  `Runtime/Streams/ZLinkSessionActorCoordinator.cs:52-62,542-626,745-755`). net8.0·net10.0 Release unit aggregate는
  각각 1,605/1,605, identifier·replacement focused test는 각각 61/61, public contract test는 각각 76/76로
  통과했다. `verify_packaged_contract.sh`는 9개 package, standalone HTTP clean consumer와 API snapshot
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를 통과했다. Worktree service-wire
  validator, generated asset check와 decoder fixture도 각각 `42 commands`, `commands=42/fixtures=37`,
  `boundSessionReplaced=pass`를 출력했지만 canonical command 51 source가 `main`에 없으므로 종결 증거로
  승격하지 않는다. 새 세 process ST-E2는
  `framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-124750-4127532/`에서 통과했고
  client assertion은 callback guidance가 close보다 먼저 도착했음을 확인했다. 이는 LAYER-003의 route·identity
  refactor 회귀를 보강하지만 residual owner method/state와 formal protocol·Windows·전체 process release
  gate는 남아 있다.
- **현재 candidate 최종 Codex Sol 계약 재검토**: 동일 `gpt-5.6-sol`, effort `high`가 기준 commit
  `137f2858bf7fd29f58405893473be8e773725a93`와 그 위의 dirty candidate, 모든 Gap·PARTIAL·public contract,
  exact interface·정식 spec·production runtime·owner regression·package/clean-consumer·process evidence를
  다시 대조했다. plan/source consistency는 `CLEAN`이고 새 finding은 없었다. 근거 범위가 부족하다는 Low
  1건은 `ZLinkSessionActorCoordinator.cs:745-755`와 `RuntimeIdentifierTests.cs:158-166`을 계획서에 추가한 뒤
  재검토에서 해소했다. 최종 계약 판정은 `NOT CLEAN`, 잔여는 High 4건·Medium 4건·Low 0건이다.
  수정 commit은 만들지 않았고 push하지 않았다. 재실행한 net8.0·net10.0 aggregate 1,605/1,605, focused
  61/61, contract 76/76, 9개 package·standalone HTTP clean consumer, ST-E2와 path-limited
  `git diff --check`는 모두 통과했다. canonical protocol·Windows/6 RID·전체 process gate와 LAYER-003
  residual은 여전히 미완료이며, 따라서 production-first POSDDD 2차 review는 시작하지 않았다.
- **이전 typed owner·Spot lifecycle 후속 gate**: 위 Sol review 이후 `ZLinkActorSessionRegistry`의 lookup/remove,
  Actor session binding state의 mesh 인자와 `ZLinkSpotLocationLifecycle`의 mesh/Spot 인자를 각각
  `ZLinkActorId`·`ZLinkMeshName`·`ZLinkSpotId`로 바꾸고 `RuntimeIdentifierTests`에 method-parameter assertion을
  추가했다(`Runtime/Actors/ZLinkActorSessionRegistry.cs:11-48`, `Runtime/Actors/ZLinkActorRuntimeState.cs:385-508`,
  `Runtime/Locations/ZLinkSpotLocationLifecycle.cs:12-158`, `RuntimeIdentifierTests.cs:164-184`).
  변경 뒤 net8.0·net10.0 focused 137/137, 전체 unit 각각 1,605/1,605, contract 각각 76/76,
  9개 package·standalone HTTP clean consumer·API hash `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`,
  세 process `ST-E2`를 `SpotActorTransfer/logs/20260808-134553-1874306/`에서 다시 통과했다.
  Worktree service-wire validator도 `42 commands`, generated asset `commands=42/fixtures=37`, decoder fixture
  `boundSessionReplaced=pass`로 다시 통과했지만 canonical command 51 source는 여전히 `main`에 없다.
  `DOTNET-LAYER-003`의 Actor ownership method·runtime boundary constructor·session binding table 외부 API residual은
  계속 PARTIAL이다. 또한 ManagedMeshNode transport test는 source/lifecycle fence만 검증하고 exact owner
  ID·lease·session RID·binding generation은 direct lookup만 검증하므로 `DOTNET-SESS-REPLACE-TEST-002`를
  별도 PARTIAL로 등록했다. 같은 candidate에서 `bash framework/languages/dotnet/e2e/InstanceSpot/run_e2e.sh all`은
  `InstanceSpot 'all' is not executable yet` 안내와 함께 exit 2로 끝났고, Config 14를 미완료로 유지했다. 이 내용은
  formal spec·canonical schema·보호 문서를 변경하지 않았다.
- **이전 typed owner candidate Codex Sol 재검토**: 동일 `gpt-5.6-sol`, effort `high`가 기준 commit
  `137f2858bf7fd29f58405893473be8e773725a93`와 그 위의 dirty candidate를 대상으로 모든 Gap·PARTIAL·public
  contract, exact interface·정식 spec·production runtime·owner regression·package/clean-consumer·process evidence를
  다시 대조했다. plan/source consistency는 `CLEAN`이고, 앞서 지적된 최신·직전 ST-E2 log 표현도
  `framework/doc/plan/dotnet-internals-spec-gap-report.ko.md:767,1004,1145`에서 구분되어 새 Low finding은 없다.
  계약 단계는 `NOT CLEAN`, 잔여는 High 4건·Medium 5건·Low 0건이다. Medium 5건에는
  `DOTNET-SESS-REPLACE-TEST-002`의 exact owner transport-to-runtime regression 누락이 포함되며
  (`framework/doc/plan/dotnet-internals-spec-gap-report.ko.md:811`), 기존 canonical command 51, Config 14,
  나머지 7개 process suite, release matrix, LAYER-003 residual, 보호 문서와 Windows evidence도 남아 있다.
  당시 재실행 gate는 net8.0·net10.0 unit 1,605/1,605, focused 137/137, contract 76/76, package/clean consumer,
  service-wire worktree validators, ST-E2와 path-limited `git diff --check`이며 수정 commit·push는 없었다.
  계약이 CLEAN이 아니므로 POSDDD 2차 review는 시작하지 않았다.
- **exact retired lookup 후속 gate**: 위 재검토 뒤 Actor runtime state의 production core constructor를
  `ZLinkActorId`로 고정하고 기존 문자열 생성자는 boundary adapter로만 남겼다
  (`Runtime/Actors/ZLinkActorRuntimeState.cs:6-67`). Bound-session replacement handler에서 authority Actor ID를
  한 번 변환한 뒤 coordinator와 binding table의 typed exact lookup overload를 호출하도록 연결했다
  (`Runtime/Host/ZLinkFrameworkRuntimeBoundSessionReplacement.cs:30-50`,
  `Runtime/Host/ZLinkActorBoundSessionCoordinator.cs:502-538`,
  `Runtime/Actors/ZLinkSessionActorBindingTable.cs:920-973`). RuntimeIdentifierTests에 typed constructor
  assertion도 추가했다(`RuntimeIdentifierTests.cs:158-166`). 당시 변경 뒤 net8.0·net10.0 Release focused
  137/137, 전체 unit 1,605/1,605, contract 76/76, 9개 package·standalone HTTP clean consumer와
  API hash `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를 재실행해 통과했다.
  Worktree service-wire 3개 validator도 통과했고, 최신 세 process ST-E2는
  `SpotActorTransfer/logs/20260808-134553-1874306/`에서 actor owner·이전 session owner·새 session owner
  evidence와 callback guidance-before-close assertion을 남겼다. Config 14 `all`은 여전히 exit 2이고,
  canonical command 51·Windows/6 RID·전체 process gate·exact owner transport-to-runtime regression·LAYER-003
  residual·보호 문서 blocker는 그대로다.
- **이전 candidate 후속 Codex Sol 재검토**: 동일 `gpt-5.6-sol`, effort `high`가 기준 commit
  `137f2858bf7fd29f58405893473be8e773725a93`와 그 위의 dirty candidate를 대상으로 모든 Gap·PARTIAL·public
  contract와 exact interface·정식 spec·production runtime·owner regression·package/clean-consumer·process
  evidence를 다시 대조했다. plan/source/evidence consistency는 `CLEAN`이고 새 finding은 없었다.
  계약 단계는 `NOT CLEAN`, 당시 잔여는 High 4건·Medium 5건·Low 0건이다. 당시 focused 137/137과 전체 unit
  1,605/1,605, `ST-E2` log `20260808-134553-1874306`의 callback guidance-before-close 및 세 역할 evidence,
  typed constructor와 exact-retired production 연결을 확인했다. canonical command 51, Config 14와 나머지
  process suite, net8/net10 single·multi-process와 6 RID release matrix, Windows evidence, LAYER-003 residual,
  exact owner transport-to-runtime regression과 보호 문서 blocker는 그대로 남아 있다. 수정 commit·push는
  없으며, 계약이 CLEAN이 아니므로 production-first POSDDD 2차 review는 시작하지 않았다.
- **Actor ownership typed-core 및 timer scheduling 재검증**: 위 검토 뒤 `IZLinkActorLocationLifecycle`와
  `ZLinkActorOwnershipCoordinator`의 claim·authority·release core parameter, relocation session state의
  target mesh를 `ZLinkActorId`·`ZLinkMeshName`·`ZLinkSpotId`로 고정하고 문자열 overload는 명시적인 legacy
  boundary adapter로만 남겼다(`Runtime/Locations/IZLinkActorLocationLifecycle.cs:1-30`,
  `Runtime/Locations/ZLinkActorOwnershipCoordinator.cs:39-120,195-349,351-799,917-1166,1324-1661`,
  `Runtime/Actors/ZLinkActorRuntimeState.cs:851-899`). `RuntimeIdentifierTests`에는 interface와 owner core
  method parameter assertion을 추가했다(`RuntimeIdentifierTests.cs:185-246`). Production build는 net8.0과
  net10.0에서 warning/error 0이고, owner/lifecycle identifier focused filter는 각 TFM 235/235,
  replacement owner filter는 각 TFM 20/20으로 통과했다. Fake `TimeProvider` test는 `TimerScheduled` signal만
  기다리지 않고 `ActiveTimerCount == 1`을 확인한 뒤 시간을 진행하도록 고쳐 scheduler registration race를
  제거했다(`BoundSessionReplacementLifecycleTests.cs:31-63,89-138`). 이 수정은 formal spec·canonical
  protocol source·보호 문서를 변경하지 않았다. 다만 compatibility boundary allowlist의 전체 Sol 재검토,
  canonical command 51·Windows/6 RID·전체 process gate·exact owner transport-to-runtime regression과
  보호 문서 blocker가 남아 `DOTNET-LAYER-003`은 여전히 PARTIAL이고 계약 단계는 NOT CLEAN이다.
- **최신 dirty candidate gate 재실행**: 위 코드·test 변경 이후 net8.0과 net10.0 전체 unit을 각각
  1,605/1,605, 전체 contract를 각각 76/76으로 다시 통과했다. `verify_packaged_contract.sh`는 9개 package,
  standalone HTTP clean consumer와 API snapshot hash `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를 통과했다.
  Worktree service-wire validator 3개는 `42 commands`, `commands=42/fixtures=37`,
  `boundSessionReplaced=pass`로 통과했다. 최신 `SpotActorTransfer ST-E2`는
  `framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-142226-2679994/`에서 Actor owner,
  이전 session owner, 새 session owner 세 process evidence와 callback guidance-before-close를 남겼다.
  Config 14 `InstanceSpot/run_e2e.sh all`은 여전히 exit 2이며, canonical command 51·Windows/6 RID·전체
  process gate·exact owner transport-to-runtime regression·보호 문서 blocker가 남아 계약 단계는 NOT CLEAN이다.
  이 gate들은 새 동일 Sol review의 기준으로 사용하며 POSDDD 2차 review는 계속 대기한다.
- **직전 dirty candidate 동일 Codex Sol 계약 review**: 동일 `gpt-5.6-sol`, effort `high`가
  기준 commit `137f2858bf7fd29f58405893473be8e773725a93`와 현재 dirty candidate를 대상으로 모든
  Gap·PARTIAL·public contract, exact interface·정식 spec·production runtime·owner regression·package/
  clean-consumer·process evidence를 다시 대조했다. plan/source/evidence consistency는 `CLEAN`이고 새
  correctness finding은 없었다. 이후 transport callback-order race를 별도 수정하기 전의 판정으로,
  계약 판정은 `NOT CLEAN`, 잔여는 **High 4 / Medium 5 / Low 0**였다.
  High는 canonical command 51 통합, Config 14, 나머지 7개 process suite, 양쪽 TFM single/multi-process와
  6 RID release matrix 미실행이다. Medium은 `DOTNET-LAYER-003` compatibility/serialization boundary
  allowlist audit, exact owner ID·lease·session RID·binding generation의 transport-to-runtime regression
  race, 보호된 .NET exact interface와 공통 implementation-gap 상태, Windows PowerShell evidence 미실행이다.
  최신 unit `1,605/1,605`, owner/lifecycle focused `235/235`, replacement owner `20/20`, contract `76/76`,
  package/clean consumer, worktree wire validator와 `ST-E2` evidence는 현재 문서와 일치한다. Medium 이상과
  미실행 필수 gate가 남았으므로 POSDDD 2차 review는 시작하지 않았고, 파일 수정·commit·push도 하지 않았다.
- **blocker 기록 후 독립 owner regression 진행**: 사용자가 지시한 blocker 처리 규칙에 따라
  `DOTNET-CONTRACT-INTEGRATION-001`을 formal protocol 승인 대기로 상세 기록하고 canonical schema·golden은
  수정하지 않았다. 그와 독립된 `DOTNET-SESS-REPLACE-TEST-002`를 진행해 두 개의 실제 `ZLinkFrameworkRuntime`과
  TCP MeshNode transport를 연결하고, `ZLinkSpotNodeRuntime` handler에서 Framework exact retired lookup까지
  정상 record와 owner ID·lease·session RID·binding generation 변조 record를 검증했다
  (`tests/Zlink.Framework.UnitTests/Runtime/BoundSessionReplacementLifecycleTests.cs:155-231`). 새 test 단독은
  net8.0·net10.0 각각 1/1, replacement·wire·identifier focused 합계는 양쪽 TFM 각각 14/14로 통과했다.
  이 결과로 `DOTNET-SESS-REPLACE-TEST-002`는 완료로 올렸지만 canonical command 51 blocker와 Windows·6 RID·전체
  process gate는 그대로 열려 있다.
- **transport fence 보강 재검증 (2026-08-08)**: 위 transport test의 invalid record 표를 확장해 forged source
  RID, source lifecycle generation, receiving owner lifecycle generation도 실제 MeshNode receiver가 거부하는지
  확인했다(`BoundSessionReplacementLifecycleTests.cs:177-232`). valid source admission은 실제 peer lifecycle
  generation을 사용하고, 잘못된 일곱 owner/source fence record 뒤에도 callback count와 disconnect count가
  변하지 않은 상태에서 actor-b valid record만 처리한다. net8.0 focused test는 19/19로 통과했다.
  이 결과는 direct table lookup이 아니라 `ProcessInfrastructureControl` →
  `IsValidBoundSessionReplacedSource` → `TryHandleBoundSessionReplacedNotification` 경계를 포함하지만,
  Windows/6 RID와 전체 process gate는 여전히 별도 조건이다.
- **transport fence callback-order 재검증 (2026-08-08)**: invalid record가 valid actor-b callback과 같은
  count를 만들어 test를 조기 통과하지 못하도록 callback actor ID queue와 actor-c serial barrier를 추가했다.
  actor-a 최초 valid callback 뒤 invalid 7건을 보내고 actor-b·actor-c valid callback을 순서대로 기다린 뒤
  callback actor ID가 정확히 `actor-a, actor-b, actor-c`인지 확인한다
  (`BoundSessionReplacementLifecycleTests.cs:218-263,431-469`). net8.0 replacement/transport focused filter
  19/19로 통과했고, 전체 unit `FullyQualifiedName!~Slow` 1,605/1,605도 통과했다. 이는 TEST-002의 race finding을 수정한
  회귀 증거이지만, 동일 검토에서 판정을 갱신하기 전까지 보수적으로 해당 Medium을 유지한다.
- **post-race aggregate gate (2026-08-08)**: callback-order barrier가 포함된 최신 net8.0 binary로
  `FullyQualifiedName!~Slow` aggregate를 다시 실행해 1,605/1,605를 통과했다. 전체 출력은
  `/tmp/zlink-dotnet-unit-net8-20260808-after-fence.log`에 남겼고, negative configuration가 출력한
  의도된 startup failure log 외의 test failure는 없었다.
- **race 수정 후 동일 Codex Sol 재검토**: 동일 `gpt-5.6-sol`, effort `high`가 callback actor ID queue와
  actor-b·actor-c serial barrier, forged source/owner fence 7건을 현재 source와 대조했다. invalid callback이
  count를 속여 조기 통과하는 경로가 제거됐고 actor ID 순서 assertion이 실제 transport/session executor
  ordering을 확인하므로 `DOTNET-SESS-REPLACE-TEST-002`를 완료로 유지했다. 새 finding은 없으며 현재
  계약 판정은 **NOT CLEAN — High 4 / Medium 4 / Low 0**이다. 잔여 Medium은 `DOTNET-LAYER-003`, 보호된
  exact interface와 common implementation-gap 상태, Windows PowerShell evidence다. canonical command 51,
  InstanceSpot, 전체 process Gap/PARTIAL, release matrix High 4와 StoreFailure·RuntimeMonitoring·
  SubmitAdmission의 구체적 process blocker는 그대로다. Medium 이상과 미실행 필수 gate가 남아
  POSDDD 2차 review는 시작하지 않았다.
- **independent aggregate 재검증**: 새 transport-to-runtime test를 포함한 전체 UnitTests를 parallel contention
  없이 net8.0과 net10.0에서 순차 실행했다(`FullyQualifiedName!~Slow`). 양쪽 결과가 각각 1,605/1,605로 통과했고,
  negative configuration test가 출력한 의도된 startup failure log 외에 test failure는 없었다. 이 gate는
  replacement regression 추가가 aggregate에 영향을 주지 않음을 확인하지만 package/clean-consumer와 process
  release matrix의 미실행 조건, canonical protocol blocker를 대체하지 않는다.
- **fresh package/clean-consumer 재검증**: 현재 dirty candidate에서
  `framework/languages/dotnet/scripts/verify_packaged_contract.sh`를 다시 실행했다. 9개 NuGet package와
  standalone HTTP clean consumer가 통과했고 public API snapshot은
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`로 유지됐다. 이 결과는 package 계약의
  현재 source 일치를 확인하지만, canonical command 51 통합·process/release matrix·Windows evidence의
  blocker를 해소하지 않는다.
- **identifier 변경 후 fresh gate 재검증 (2026-08-08)**: `ZLinkRouteMeshRuntimeService`의 mesh registry와
  `ZLinkStreamSessionTable`의 `RoutingId` key를 포함한 candidate로 net8.0 전체 unit
  `FullyQualifiedName!~Slow`를 다시 실행해 `1,605/1,605`를 통과했다
  (`/tmp/zlink-dotnet-unit-net8-20260808-after-identifiers.log`). 같은 candidate의 package verifier도
  9개 package와 standalone HTTP clean consumer를 통과했고 snapshot은
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`로 동일했다
  (`/tmp/zlink-dotnet-package-20260808-after-identifiers.log`). RouteMesh monitoring의 `MON-A4B` actual
  process도 통과했다(`RuntimeMonitoring/logs/20260808-162800-2089091/`). STREAM session을 포함한
  `ChannelEgressRouting all`은 처음 `CH-E2E-04C`에서 고정 HTTP port `2683` 충돌로 실패했지만
  (`logs/20260808-162918-2179224/play.stderr.log`), 같은 candidate 단독 `CH-E2E-04C`와 이후 all
  재실행이 각각 통과했다. 최종 all 결과는 26/26이며 마지막 scenario evidence는
  `logs/20260808-163307-2365531/`와 `/tmp/zlink-dotnet-channel-egress-20260808-after-identifiers-rerun.log`에
  남겼다. 이 결과는 최신 identifier binary의 unit/package/두 process 회귀 freshness를 보강하지만,
  net10·6 RID·Windows와 나머지 process suite gate를 대체하지 않는다.
- **latest same-Sol contract review (2026-08-08)**: 동일 `gpt-5.6-sol`, effort `high`가 기준 commit
  `137f2858bf7fd29f58405893473be8e773725a93`와 위 fresh gate 이전의 dirty candidate를 다시 대조해
  `NOT CLEAN — High 4 / Medium 5 / Low 1`로 판정했다. 새 Medium은 identifier production 변경 뒤
  aggregate/package/RouteMesh·STREAM process 증거가 stale였다는 freshness finding이며, 위 fresh gate로
  net8·package·두 process lane을 갱신했다. 기존 `DOTNET-LAYER-003` Medium은 MonitorHub가 typed mesh를
  `.Value`로 풀어 private sequence method에 넘기던 경계 왕복이었다. 이를
  `NextSequence(ZLinkMeshName)` overload로 수정했으며 다음 동일 Sol review에서 해소 여부를 재확인한다.
  Low는 같은 경계 설명의 문서 불일치였고 checkpoint 문장을 수정했다. High 4와 보호 문서·Windows
  evidence Medium은 그대로 blocker로 남아 있으며, POSDDD 2차 review는 시작하지 않았다.
- **typed overload 후 aggregate flaky test 재검증 (2026-08-08)**: typed `NextSequence(ZLinkMeshName)`를
  포함한 test binary의 첫 aggregate에서 `EntrySpotActorDispatchTests.MessageFollowRemoteReplyRelay_DoesNotSubmitAfterDeadline`
  한 건이 20 ms wall-clock deadline과 60 ms delay의 scheduling race로 실패했다(`EntrySpotActorDispatchTests.cs:6055-6076`,
  `/tmp/zlink-dotnet-unit-net8-20260808-after-nextsequence.log`). 이 failure는 identifier runtime이나
  public contract failure가 아니며, deadline semantics를 바꾸지 않는 test synchronization margin만
  100 ms/300 ms로 넓혔다. 변경 후 focused test를 3회와 detailed runner 1회(`533 ms`) 통과시켰고, 최신
  candidate 전체 net8 aggregate도 `1,605/1,605`로 통과했다
  (`/tmp/zlink-dotnet-deadline-test-20260808.log`, `/tmp/zlink-dotnet-unit-net8-20260808-after-deadline-margin.log`).
  test 안정화는 계약 blocker를 닫지 않으며 net10·6 RID·Windows와 나머지 process gate는 계속 미실행이다.
- **final same-Sol contract review (2026-08-08)**: 동일 `gpt-5.6-sol`, effort `high`가 deadline test
  margin과 최신 net8 aggregate `Failed 0 / Passed 1,605`를 포함한 current dirty worktree를 다시 검토했다.
  기준 commit은 `137f2858bf7fd29f58405893473be8e773725a93`이며 새 finding은 없고 `Low 0`으로 확인했다.
  최종 계약 판정은 **NOT CLEAN — High 4 / Medium 5 / Low 0**이다. High는 canonical command 51 formal
  integration, Config 14 InstanceSpot, 전체 process Gap/PARTIAL, 양쪽 TFM single/multi-process와 6 RID
  release matrix다. Medium은 `DOTNET-LAYER-003` 전체 compatibility/serialization allowlist audit, 보호된
  exact interface 상태, 보호된 common implementation-gap 상태, Windows PowerShell evidence와 current
  identifier candidate의 net10 aggregate freshness다. typed `NextSequence` 경계와 plan line 근거는 source와
  일치하며, 계약 단계가 CLEAN이 아니므로 POSDDD 2차 review는 시작하지 않았다.
- **final same-Sol line-consistency recheck (2026-08-08)**: 동일 `gpt-5.6-sol`, effort `high`가 기준 commit
  `137f2858bf7fd29f58405893473be8e773725a93`와 current dirty worktree를 read-only로 다시 대조했다. 직전
  Low finding이었던 relocation target-stage assertion 근거를 `RuntimeIdentifierTests.cs:167-171`로 수정한
  문서와 실제 assertion이 일치하고, RelocationRuntime `171/171`, RuntimeIdentifier `3/3`, net8 aggregate
  `1,605/1,605`, 9개 package와 standalone clean consumer, path-limited `git diff --check` 결과를 확인했다.
  새 finding은 없으며 최종 계약 판정은 **NOT CLEAN — High 4 / Medium 5 / Low 0**이다. 남은 High는
  canonical command 51 formal integration, Config 14 InstanceSpot, 전체 process Gap/PARTIAL, 양쪽 TFM
  single/multi-process와 6 RID release matrix다. 남은 Medium은 `DOTNET-LAYER-003` 전체
  compatibility/serialization allowlist audit, 보호된 exact interface와 common implementation-gap 상태,
  Windows PowerShell evidence, current identifier candidate의 net10 aggregate freshness다. 보호 문서와
  canonical protocol에는 승인 없는 수정이 없으며 계약 단계가 CLEAN이 아니므로 POSDDD 2차 review는 시작하지 않았다.
- **same-Sol review after managed-object/channel key changes (2026-08-08)**: 동일 `gpt-5.6-sol`, effort `high`가
  기준 commit `137f2858bf7fd29f58405893473be8e773725a93`과 current dirty worktree를 read-only로 다시 대조했다.
  `_actors`, `_spots`, `_channels`의 `ZLinkActorId`, `ZLinkSpotId`, `ZLinkChannelName` key와 boundary descriptor
  serialization에 새 correctness finding은 없었다. RuntimeIdentifier `3/3`, ServiceRuntimeFoundation
  `48/48`, identifier/weight/foundation focused `65/65`, relocation·entry spot `308/308`, channel key 이후
  net8.0 aggregate `1,605/1,605`(`/tmp/zlink-dotnet-unit-net8-20260808-after-channel-key.log`)와
  path-limited `git diff --check`를 확인했다. 다만 최신 source 변경 뒤 package/clean-consumer와 관련 ST-E2는
  이전 candidate evidence이므로 freshness gate로 승격하지 않았다. 최종 계약 판정은 **NOT CLEAN — High 4 /
  Medium 5 / Low 1**이다. High는 canonical command 51 formal integration, Config 14 InstanceSpot, 전체
  process Gap/PARTIAL, 양쪽 TFM single/multi-process와 6 RID release matrix다. Medium은
  `DOTNET-LAYER-003` 전체 compatibility/serialization boundary allowlist, 보호된 exact interface 상태,
  보호된 common implementation-gap 상태, Windows PowerShell evidence와 current-candidate
  net10 aggregate·package/clean-consumer·관련 ST-E2 freshness다. Low 1은 plan의 이전 package/ST-E2
  evidence를 current candidate로 읽을 수 있는 consistency 잔여이며, 해당 gate를 current candidate에서
  재실행하거나 미실행 범위를 명시하기 전에는 계약 단계 CLEAN 및 POSDDD 2차 review를 시작하지 않는다.
- **current-candidate ST-E2 assertion-race fix (2026-08-08)**: 최신 `_actors`/`_spots`/`_channels` candidate에서
  `SpotActorTransfer ST-E2`를 재실행하자 role evidence 순서는 callback guidance 후 disconnect였지만, client가
  `await replacementGuidance`의 continuation 뒤에 flag를 기록해 `Disconnected` callback이 먼저 관찰하는
  assertion race가 재현됐다(`logs/20260808-172651-4003770/`, `StE2BoundSessionRebindIsolationScenario.cs:23-75`).
  production callback/close 순서나 스펙을 바꾸지 않고, disconnect lifecycle boundary에서 wait task의
  `IsCompletedSuccessfully`를 직접 확인하도록 E2E assertion만 고쳤다. 수정 후 같은 current candidate의
  `ST-E2`가 `operation SpotActorTransfer.ST-E2 passed`로 통과했고, 세 process evidence는
  `SpotActorTransfer/logs/20260808-172823-4022771/`에 남았다. 이 결과로 관련 ST-E2 freshness를 갱신하지만
  전체 release/process/Windows gate는 여전히 열려 있다.
- **current-candidate net10 aggregate freshness (2026-08-08)**: 같은 `_actors`/`_spots`/`_channels` candidate를
  `/tmp/zlink-dotnet10/dotnet` SDK `10.0.302`로 복원한 뒤 `-f net10.0 -p:ZLinkFrameworkTargetFrameworks=net10.0
  --filter 'FullyQualifiedName!~Slow' --no-restore -c Release --maxcpucount:1`를 실행했다. 초기 restore 전
  실행은 assets에 `net10.0` target이 없어 `NETSDK1005`로 중단됐고, 소스·계약 실패가 아닌 환경 준비 문제로
  기록했다. net10 target을 명시한 restore 후 재실행한 현재 후보의 결과는 `1,605/1,605` 통과이며
  `/tmp/zlink-dotnet-unit-net10-20260808-after-channel-key.log`에 남겼다. 따라서 current candidate 기준
  net8/net10 aggregate, 9개 package와 standalone clean consumer, 관련 ST-E2 evidence가 모두 갱신됐다.
  다만 양쪽 TFM single/multi-process, 6 RID release matrix와 Windows PowerShell evidence, 전체 process
  Gap/PARTIAL은 여전히 미실행 또는 실패 상태다.
- **current-candidate identifier-residual aggregate refresh (2026-08-08 18:10)**: Actor leaving registry와
  bound-session submitter cache의 typed-key 수정 뒤 net8.0 Release aggregate를
  `/tmp/zlink-dotnet-unit-net8-20260808-after-identifier-residual.log`에서 1,605/1,605로 통과했다.
  net10.0은 첫 aggregate에서 기존 `ZLinkAsyncSubmitterTests.DisposeAsync_Twice_RacingReady_CleansPendingResourcesOnce`
  race가 발생해 runner가 정리되지 않았고, 해당 test 단독 재실행은 1/1 통과했다. 같은 build를
  `--no-build` serial로 다시 실행한 net10.0 aggregate는
  `/tmp/zlink-dotnet-unit-net10-20260808-after-identifier-residual-rerun.log`에서 1,605/1,605로
  통과했다. 첫 flaky 결과는 gate failure evidence로 보존하며, 두 TFM의 current-candidate unit
  aggregate freshness는 두 번째 실행 결과로 갱신한다. 이 결과도 양쪽 TFM single/multi-process, 6 RID,
  Windows와 전체 process Gap/PARTIAL을 닫지 않는다.
- **current-candidate package/clean-consumer refresh (2026-08-08 18:16)**: 같은 residual-identifier source에서
  `framework/languages/dotnet/scripts/verify_packaged_contract.sh --generate-snapshot`을 다시 실행했다.
  9개 NuGet package와 standalone HTTP clean consumer가 통과했고 snapshot은
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`로 유지됐다. Review snapshot
  directory는 `/tmp/zlink-dotnet-review-identifier-residual.dMaxWv/`이고 raw output은
  `/tmp/zlink-dotnet-package-20260808-after-identifier-residual.log`다. 이 결과는 typed-key 수정이
  package contract와 clean consumer를 깨지 않았음을 확인하지만, command 51 canonical integration,
  Windows/RID release와 process Gap/PARTIAL blocker는 그대로다.
- **이전 동일 Codex Sol 계약 재검토 (2026-08-08, residual 이전 candidate)**: 동일 `gpt-5.6-sol`, effort `high`가 기준 commit
  `137f2858bf7fd29f58405893473be8e773725a93`와 current dirty worktree를 대상으로 모든 .NET Framework
  Gap·PARTIAL·public contract를 exact interface·정식 spec·production runtime·owner-layer regression·
  package/clean-consumer·process evidence와 다시 대조했다. 스펙·보호 문서는 수정하지 않았고 새 correctness
  finding은 없었다. 당시 net8/net10 aggregate `1,605/1,605`, 9개 package와 standalone HTTP clean
  consumer, API snapshot `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`,
  ST-E2 callback-guidance-before-close evidence와 path-limited `git diff --check`가 당시 candidate와
  일치했다. 이후 identifier residual source가 추가되어 이 ST-E2 evidence는 current-candidate gate로
  승격하지 않는다. 당시 계약 판정은 **NOT CLEAN — High 4 / Medium 4 / Low 0**이다. High는 canonical command 51
  formal integration, Config 14 InstanceSpot, 전체 process Gap/PARTIAL, 양쪽 TFM single/multi-process와
  6 RID release matrix다. Medium은 `DOTNET-LAYER-003` 전체 compatibility/serialization boundary
  allowlist audit, 보호된 .NET exact interface 상태, 보호된 common implementation-gap 상태, Windows
  PowerShell evidence다. 따라서 POSDDD 2차 review는 시작하지 않았다.
- **동일 Codex Sol 재검토 — residual/HTTP/lifecycle 반증 (2026-08-08 18:20)**: 동일
  `gpt-5.6-sol`, effort `high`가 기준 commit `137f2858bf7fd29f58405893473be8e773725a93`와
  current dirty worktree를 대상으로 모든 .NET Framework Gap·PARTIAL·public contract, exact interface,
  정식 spec, production runtime, owner-layer regression, package/clean-consumer와 process evidence를 다시
  대조했다. 리뷰어는 파일을 수정하지 않았으며 POSDDD 2차 review도 시작하지 않았다.
  새 High는 `DOTNET-HTTP-SERIALIZATION-001`이다. 기존 global `ActorRef` JSON의 `nodeRid` 문자열 계약
  (`common/spec/04-message-model.ko.md:70-92`)을 `Zlink.HttpClient` 기본 decoder가 converter 없이 처리해
  TA-A4가 `ActorRef.ValidateNodeRid`에서 실패한 것을 실제 log와 source에서 확인했다
  (`Zlink.HttpClient/Runtime/HttpClientCodecRegistry.cs:10,65,83`,
  `ToActorMessaging/logs/20260808-174302-428557/client.stderr.log:1`). 이는 spec 변경 사유가 아니며
  server/HTTP client package 경계의 기존 serializer 구현과 exact round-trip/process gate가 필요하다.
  `DOTNET-LAYER-003`에는 `ZLinkMeshChannelSelection`과 `ZLinkFanoutRuntimeService`의 raw channel key가
  추가 residual로 지적되었고, 이를 `ZLinkChannelName`으로 바꾼 뒤 net8 focused 23/23을 통과했다. 그러나
  current source 이후 ST-E2가 다시 실행되지 않아 replacement process freshness Medium이 재개됐다
  (`ST-E2 logs/20260808-172823-4022771/`는 residual 이전 candidate). 또한 최신 net10 첫 aggregate의
  `ZLinkAsyncSubmitter.OnSendReady`와 Dispose 경쟁에서 `ObjectDisposedException`이 callback task 밖으로
  전파된 기록을 확인해 `DOTNET-SUBMITTER-DISPOSE-RACE-001` Medium을 추가했다
  (`Runtime/Messaging/ZLinkAsyncSubmitter.cs:456-465,603-670`,
  `tests/Zlink.Framework.UnitTests/Runtime/ZLinkAsyncSubmitterTests.cs:579-585`,
  `/tmp/zlink-dotnet-unit-net10-20260808-after-identifier-residual.log:12-26`). isolated/rerun pass는
  경쟁 부재만 보여 주므로 종결 근거로 승격하지 않는다. 최종 계약 판정은 **NOT CLEAN — High 5 /
  Medium 6 / Low 1**이며, Low는 plan의 이전 ST-E2 current 표기와 이후 freshness 보류 문장 불일치다.
  새 source 수정·current ST-E2·HTTP serializer round-trip·submitter race 회귀와 관련 gate를 다시 실행한 뒤
  동일 Sol review를 반복한다. 보호 spec과 canonical protocol은 수정하지 않았다.
- **동일 Codex Sol 재검토 — current gate 종결 확인 (2026-08-08 18:45)**: 동일 `gpt-5.6-sol`, effort
  `high`가 기준 commit `137f2858bf7fd29f58405893473be8e773725a93`와 current dirty worktree를 다시
  검토했다. 동일 reviewer는 파일을 수정하지 않았고, path-limited `git diff --check`와 최신 net8/net10
  aggregate `1,605/1,605`, 9개 package·standalone HTTP clean consumer, current ST-E2 세 process evidence를
  확인했다. `DOTNET-SUBMITTER-DISPOSE-RACE-001`은 ready callback의 dispose 예외를 pending terminal로
  수렴시키는 production 경계와 dispose 뒤 callback/cleanup-once regression으로 완료됐고,
  `DOTNET-SESS-REPLACE-FRESHNESS-001`은 `SpotActorTransfer/logs/20260808-182644-2031944/`에서
  callback guidance-before-close와 Actor owner·이전 session owner·새 session owner evidence를 확인해
  완료됐다. channel/fanout raw key residual도 `ZLinkChannelName`으로 해소됐다. 새 correctness finding은
  없으며 계약 판정은 **NOT CLEAN — High 5 / Medium 4 / Low 0**이다. 남은 High는 canonical command 51
  formal integration(동일 blocker), `DOTNET-HTTP-SERIALIZATION-001`, Config 14 InstanceSpot, 전체 process
  Gap/PARTIAL, 양쪽 TFM single/multi-process와 6 RID release matrix다. 남은 Medium은
  `DOTNET-LAYER-003` 전체 compatibility/serialization boundary allowlist, 보호된 .NET exact interface와
  common implementation-gap 상태, Windows PowerShell evidence다. canonical protocol과 보호 문서는 수정하지
  않았으며, 계약 단계가 CLEAN이 아니므로 POSDDD 2차 review는 시작하지 않았다.
- **2차 review**: 1차 계약 review가 CLEAN이 아니므로 production-first POSD/DDD review를 시작하지 않았다.
  Pre-review에서 찾은 replacement admission retry의 반복 encode·diagnostic 비용은 2차 review 후보로만 남겼다.
- **StoreFailure process blocker (2026-08-08)**: `framework/languages/dotnet/e2e/StoreFailure/run_e2e.sh all`이
  SF-B1에서 실패했다. 기준 commit은 `137f2858bf7fd29f58405893473be8e773725a93`이며, 재현 log는
  `framework/languages/dotnet/e2e/StoreFailure/logs/20260808-153132-482267/`와
  `framework/languages/dotnet/e2e/StoreFailure/logs/20260808-154322-744443/`이다. Redis pause 뒤
  consumer의 1~8번째 요청은 성공했지만 9번째 요청은 `SubmitNativeApplicationRequest`가 target API-A와
  physical RID를 확인한 뒤 accepted로 반환되었고, provider에는 request가 도착하지 않아 3초(진단 run에서는
  10초) 후 `TimedOut`이 됐다. API-A와 API-B에는 `autoconnect_tick_failed`만 기록됐고 의도하지 않은 peer
  remove는 shutdown 때까지 관찰되지 않았다. 따라서 현재 증거만으로는 StoreFailure 계약이나 public spec을
  변경할 근거가 없으며, 이 항목은 **runtime/transport 원인 미확정 BLOCKER**로 남긴다.
  진단을 위해 `SfProbe` timeout을 10초로 늘리고 SF-A1 반복 횟수를 12회로 바꾼 변경 및 submit/complete
  debug log는 임시 변경이며, SF-A1 정상 run(`logs/20260808-154553-763564`)에서 12회가 모두 통과한 뒤
  원래 timeout 3초·반복 8회와 debug 없는 production source로 복원한다. 다음 독립 작업은 provider
  receive/dispatch와 native transport monitor의 경계를 확인하고, 원인이 확인되기 전에는 timeout 완화,
  재시도 추가, formal spec 변경으로 실패를 숨기지 않는 것이다.
- **StoreFailure SF-B1 transport 경계 재현 (2026-08-08)**: 임시 `application_request_submit`/
  `application_request_result` 진단을 켠 재실행(`framework/languages/dotnet/e2e/StoreFailure/logs/20260808-170233-3498304/`)에서
  요청 1~9는 API-A/API-B에 번갈아 도착해 `Ok`로 끝났고, 요청 10은 target과 physical RID가 모두 API-B로
  선택되어 native submit까지 `Ok`로 반환됐지만 API-B provider에는 도착하지 않은 채 `TimedOut`이 됐다.
  실패 시점에도 consumer route status는 `api-a|Ready;api-b|Ready`, `selectable=True`였고, API-B peer의
  의도하지 않은 close/remove는 shutdown까지 없었다. 이 결과는 route selection과 managed submit의
  admission만 증명하며 provider receive/dispatch 또는 Core request delivery의 원인은 아직 확정하지
  못한다. 따라서 StoreFailure 계약·정식 spec을 변경하지 않고 **runtime/transport 원인 미확정 BLOCKER**로
  유지한다. 진단 로그는 원인 확인 후 production source에서 제거하고 build와 SF-B1 회귀를 다시 실행한다.
- **RuntimeMonitoring process blocker (2026-08-08)**: Linux net8.0에서
  `framework/languages/dotnet/e2e/RuntimeMonitoring/run_e2e.sh all`을 실행한 결과
  `MON-A1`부터 일부 시나리오는 통과했지만 `MON-A4B`에서 exit 134로 중단됐다. 증거는
  `framework/languages/dotnet/e2e/RuntimeMonitoring/logs/20260808-155034-904906/`이며,
  client가 `MonA4AvailabilityTransitionScenario.cs:97,134,165`에서 `MON-A4 peer 'svc-b' did not become ready`
  예외를 냈다. provider의 의도된 failing timer 로그는 시나리오 검증 대상이지만, crash recovery 후 `svc-b`
  ready 전이가 완료되지 않은 것은 필수 process evidence를 충족하지 못한다. 현재 결과만으로 public spec이나
  formal protocol을 변경할 근거는 없으므로 **process/runtime blocker**로 기록한다. 재실행 시에는 isolated
  `MON-A4B`에서 peer lifecycle evidence와 service-b process 상태를 먼저 수집하고, readiness timeout을
  임의로 늘려 실패를 감추지 않는다. 이 blocker와 독립된 unit·package·다른 process suite gate는 계속 진행한다.
- **independent RegistrationCodec process gate (2026-08-08)**: Linux net8.0에서
  `framework/languages/dotnet/e2e/RegistrationCodec/run_e2e.sh all`을 재실행해 RC-A1~RC-A6와 RC-B1~RC-B6,
  총 12개 scenario가 모두 통과했다. 실행 log는
  `framework/languages/dotnet/e2e/RegistrationCodec/logs/20260808-155132-942270/`이고 summary는
  `registration-codec e2e result=passed`이다. 이 결과는 RegistrationCodec process evidence를 보강하지만,
  command 51 canonical blocker와 다른 process/RID/Windows gate를 종결하지 않는다.
- **SubmitAdmission process blocker (2026-08-08)**: Linux net8.0에서
  `framework/languages/dotnet/e2e/SubmitAdmission/run_e2e.sh all`이 readiness 단계에서 exit 22로 실패했다.
  log는 `framework/languages/dotnet/e2e/SubmitAdmission/logs/20260808-155202-948852/`이고,
  caller가 `GET /ready/submit-target` 호출 중
  `ZLinkFrameworkRuntime.EnsureKnownRouteMeshPeer`(`Runtime/Host/ZLinkFrameworkRuntimeChannels.cs:200`)에서
  `Route channel 'submit-admission.mesh' is not connected to node 'submit-target' for packet 'RouteReadyRequest'`
  를 받아 HTTP 500을 반환했다. target·publisher·object-client health는 기동했지만 caller의 route
  admission이 완료되지 않았다. 이는 process/transport readiness 증거 부족이며 public spec이나 protocol을
  변경할 근거가 없으므로 **process blocker**로 남긴다. isolated 재실행에서 peer admission event와
  readiness polling 경계를 확인한 뒤에만 runner/runtime 수정 여부를 판단하고, readiness timeout 완화로
  실패를 숨기지 않는다.
- **local net10 rerun limitation (2026-08-08)**: 현재 shell의 SDK는 `8.0.129`이므로
  `dotnet restore ... -p:ZLinkFrameworkTargetFrameworks=net10.0`가 `NETSDK1045`로 중단됐다.
  이 결과는 net10 계약 결함의 증거가 아니라 현재 환경에서의 재실행 불가 증거이며, 별도 SDK `10.0.302`를
  사용하는 기존 Linux x64 aggregate evidence를 대체하지 않는다. 따라서 net10 single/multi-process와
  6 RID release gate는 계속 **미실행**으로 유지하고, SDK 10 환경에서 다시 실행한다.
- **independent ChannelEgressRouting process gate (2026-08-08)**: Linux net8.0에서
  `framework/languages/dotnet/e2e/ChannelEgressRouting/run_e2e.sh all`을 실행해 CH-E2E-01~12,
  CH-REG-01~10과 변형 scenario를 포함한 총 26개가 통과했다. 실행 출력은
  `/tmp/zlink-dotnet-channel-egress-20260808.log`이며 마지막 summary는
  `channel-egress-routing e2e result=passed scenarios=26`이다. 이 evidence는 matrix의 기존
  `all exits 2` 표기가 stale임을 확인하지만 canonical command 51·InstanceSpot·StoreFailure·다른
  process/RID/Windows gate를 종결하지 않는다.
- **independent PubSub process gate (2026-08-08)**: Linux net8.0에서
  `framework/languages/dotnet/e2e/PubSub/run_e2e.sh all`을 실행해 PS-A1~PS-A4, PS-B1~PS-B2, PS-C1 총
  7개 scenario가 통과했다. log는 `framework/languages/dotnet/e2e/PubSub/logs/20260808-160554-1527118/`이고
  summary는 `pubsub e2e result=passed`다. 이 결과는 PubSub process evidence를 보강하지만 전체 process
  Gap/PARTIAL와 release/Windows gate를 닫지 않는다.
- **ToActorMessaging process blocker (2026-08-08)**: Linux net8.0에서
  `framework/languages/dotnet/e2e/ToActorMessaging/run_e2e.sh all`이 TA-A4에서 exit 134로 중단됐다.
  log는 `framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-160624-1534689/`이고,
  client `ToActorScenarioContext.cs:128`가 `TaA4DisconnectAndDestroyScenario.cs:12`에서 actor ref JSON을
  decode하는 중 `ActorRef.ValidateNodeRid`(`framework/languages/dotnet/src/Zlink.Framework/Contracts/Actors/ActorRef.cs:65`)의
  `Actor owner routing id must not be empty` 예외를 받았다. 앞선 actor/session process는 기동·일부
  evidence를 남겼지만, disconnect/destroy 후 capture 응답이 유효한 owner RID를 포함하지 않아 전체 process
  gate를 충족하지 못한다. 이는 public spec 변경 근거가 아니므로 **process/serialization blocker**로 기록하며,
  JSON 응답의 owner RID 생성 경계를 확인하기 전에는 client 우회 변환이나 계약 완화를 하지 않는다.
- **ToActorMessaging serialization blocker 재현 (2026-08-08 17:43)**: 같은 current candidate에서
  `run_e2e.sh TA-A4`를 다시 실행했지만 `logs/20260808-174302-428557/`에서 같은 경계가 재현됐다.
  `Caller/Program.cs:96`의 `Results.Ok(actor)`는 `ActorRef`를 ASP.NET 기본 JSON serializer에 넘기고,
  client는 `ToActorScenarioContext.cs:126-131`에서 `Zlink.HttpClient`의 기본 decoder를 사용한다.
  후자는 `Runtime/HttpClientCodecRegistry.cs:10,65,83`의 기본 `System.Text.Json` Web options만 사용하므로
  공통 object-reference JSON이 요구하는 `nodeRid` 문자열을 `RoutingId`로 복원하지 못해
  `ActorRef.ValidateNodeRid`(`Contracts/Actors/ActorRef.cs:65`)에서 실패한다. 서버 측도 같은 기본
  serializer를 사용하므로 `RoutingId` object shape를 반환할 수 있다. 이는 `04-message-model.ko.md:79-92`의
  기존 global object reference 계약을 바꿔야 한다는 근거가 아니며, Framework HTTP JSON boundary의
  serializer ownership을 먼저 정해야 하는 **process/serialization blocker**다. client-side raw/object
  우회, DTO로 계약을 숨기기, validation 완화는 하지 않는다. 정식 spec 변경은 현재 필요하지 않다.
  필요한 해결 방향은 (a) Framework server와 `Zlink.HttpClient`가 기존 `framework-json-v1` object-reference
  converter를 공통으로 소유하도록 package 경계를 검토하고, (b) exact `ActorRef` round-trip unit 및
  TA-A4 process evidence를 다시 실행하는 것이다. 이 blocker와 독립적인 unit/package/다른 process gate는
  계속 진행한다.
- **DOTNET-HTTP-SERIALIZATION-001 — public ActorRef JSON 복원 실패 (High, 2026-08-08)**: TA-A4는
  단순 process evidence 누락이 아니라 기존 global object-reference JSON 계약을 public HTTP 경계에서
  복원하지 못하는 production 결함이다. 정식 계약은 `framework/doc/framework/common/spec/04-message-model.ko.md:70-92`에서
  `nodeRid`를 문자열로 요구한다. Caller는 public `ActorRef`를 `Results.Ok(actor)`로 반환하고
  (`framework/languages/dotnet/e2e/ToActorMessaging/Server/Caller/Program.cs:87-96`), client는
  `Async<ActorRef>()`로 typed decode를 요청하지만 `Zlink.HttpClient`의 기본 options가 converter 없이
  `System.Text.JsonDefaults.Web`을 사용한다(`framework/languages/dotnet/src/Zlink.HttpClient/Runtime/HttpClientCodecRegistry.cs:10,65,83`).
  최신 재현 log `framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-174302-428557/client.stderr.log:1`은
  `ActorRef.ValidateNodeRid`에서 empty RID를 거부한다. client-side raw/object 변환, DTO 우회 또는 validation
  완화는 허용하지 않는다. 기존 spec·protocol을 바꿀 근거는 없으므로 **스펙 변경 blocker가 아니다**.
  해결 조건은 server와 `Zlink.HttpClient`가 기존 `framework-json-v1` object-reference converter를 package
  책임 경계에서 함께 사용하도록 구현하고, exact `ActorRef` round-trip unit, package/clean-consumer와
  TA-A4 three-process evidence를 current candidate에서 다시 통과시키는 것이다. 해결 전에는 전체
  contract 판정을 CLEAN으로 올리지 않는다.
- **현재 candidate 해결 checkpoint (2026-08-08 18:46)**: 정식 spec·protocol 변경 없이 기존 object-reference
  계약을 타입 소유 경계에 구현했다. `ActorRef`와 `SpotRef`에 private `JsonConverter`를 연결해 property
  이름과 순서, `objectGeneration` decimal string, `nodeRid` lowercase hex string을 고정하고, duplicate·unknown·
  null·누락·numeric/leading-zero generation을 거부한다(`framework/languages/dotnet/src/Zlink.Framework/Contracts/Actors/ActorRef.cs:1-186`,
  `framework/languages/dotnet/src/Zlink.Framework/Contracts/Spots/Contracts.cs:1-157`). Framework ASP.NET
  `Results.Ok(actor)`와 standalone `Zlink.HttpClient`는 이 type-owned converter를 자동으로 사용하므로
  호출자 raw 변환이나 DTO 우회가 없다. `FrameworkJsonProfileTests`의 ActorRef/SpotRef exact round-trip 및
  malformed vector는 15/15로 통과했다(`framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/FrameworkJsonProfileTests.cs:8-50`).
  current package verifier는 9개 package·standalone HTTP clean consumer와 API snapshot
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를 통과했다
  (`/tmp/zlink-dotnet-package-20260808-http-json.log`). 최신 TA-A4 three-process run도
  `to-actor-messaging e2e result=passed`였고 caller가 `ActorRef`를 ASP.NET JSON으로 쓰고 client가
  capture 응답을 decode한 evidence가 남았다(`framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-184235-2531208/`).
  따라서 이 production serialization 결함은 **완료**이며, ToActor 전체 process·Instance Spot·canonical
  command 51·Windows/RID release gate의 미완료 상태와 분리한다.
- **동일 Codex Sol 재검토 blocker (2026-08-08 18:56, `gpt-5.6-sol`, effort `high`, 기준 commit
  `137f2858bf7fd29f58405893473be8e773725a93` + current dirty worktree)**: 유효한 object-reference
  round-trip과 TA-A4 pass만으로는 정식 JSON contract를 종결할 수 없다는 High finding이 확인됐다.
  `SpotRef`가 public contract의 UTF-8 `1..255` bytes `SpotId`를 invariant로 고정하지 않아 empty·oversized ID를
  decode할 수 있었고, 두 value type의 `default` 또는 범위 밖 generation·empty RID를 converter writer가
  contract-invalid JSON으로 내보낼 수 있었다(`Contracts/Spots/Contracts.cs:42-147`,
  `Contracts/Actors/ActorRef.cs:162-175`, common spec `04-message-model.ko.md:69-93`, exact Spot interface
  `05-spots.ko.md:10-13`). 이는 spec 변경 사유가 아니며 **스펙 변경 blocker가 아니다**. 현재 source는
  `SpotRef` constructor/property invariant와 두 converter의 encode-time validation을 추가했고,
  empty·NUL·oversized SpotId, generation·mesh·RID invalid decode 및 default encode regression을
  `FrameworkJsonProfileTests`에 추가했다(`FrameworkJsonProfileTests.cs:50-71`). 수정 후 focused gate는
  net8 `FrameworkJsonProfileTests` 22/22 PASS였지만, 이전 package/clean-consumer·TA-A4 evidence는 수정 전
  binary이므로 current candidate 증거로 승격하지 않는다. package/clean-consumer, TA-A4와 관련 aggregate를
  다시 실행하고 동일 Sol review에서 malformed contract rejection을 확인하기 전까지 이 항목은 High로
  유지한다. caller raw conversion, DTO 우회, validation 완화와 정식 spec 수정은 하지 않는다.
- **HTTP malformed-value 수정 checkpoint (2026-08-08 19:00 이후, Sol 재판정 전)**: 위 blocker의 계약 범위 안에서만
  source를 보강했다. `SpotRef`는 constructor/property에서 SpotId의 UTF-8 `1..255` bytes·NUL·generation·
  mesh·RID invariant를 검증하고, 두 type-owned converter writer는 encode 전에 같은 invariant를 다시 확인한다.
  `FrameworkJsonProfileTests`는 invalid SpotId·generation·mesh·RID와 default value encode를 포함해 22/22 PASS했고,
  identifier regression을 합친 focused run은 25/25 PASS했다. current source 기준 net8.0 Release unit aggregate는
  1,617/1,617(`/tmp/zlink-dotnet-unit-net8-20260808-http-invariant.log`), net10.0은 1,617/1,617
  (`/tmp/zlink-dotnet-unit-net10-20260808-http-invariant.log`)이다. 9개 package와 standalone HTTP clean consumer 및
  API snapshot `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`도 통과했다
  (`/tmp/zlink-dotnet-package-20260808-http-json-invariant.log`, review output `/tmp/zlink-dotnet-http-invariant-review.6Oesvg`).
  TA-A4 three-process도 `to-actor-messaging e2e result=passed`로 재실행했다
  (`framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-185901-2866287/`). 이 evidence는 같은
  candidate에 대한 Sol 재판정 전까지 **수정 검증 중**으로 기록하며, 그 전의 18:46 completion 문장은 historical
  checkpoint로 유지한다.
- **마지막 ActorRef 경계 보강으로 인한 evidence freshness blocker (2026-08-08 19:07)**: `ActorRef`의
  `ValidateActorId`에 NUL·strict UTF-8·1..255 bytes 검증을 추가하고 actor malformed JSON regression을
  늘렸다(`Contracts/Actors/ActorRef.cs:1-80`, `FrameworkJsonProfileTests.cs:50-86`). source correctness는
  동일 Sol에서 반례 없이 확인됐지만, 이 변경 뒤 focused 27/27만 실행되었고 기존 net8/net10 aggregate,
  package/clean-consumer와 TA-A4는 모두 직전 binary 증거다. 따라서 HTTP 항목을 완료로 올리거나 전체 집계를
  낮추지 않고 **High 1건과 Low freshness 1건을 blocker로 유지**한다. 마지막 source를 포함한 net8/net10
  aggregate, package/clean-consumer, TA-A4를 다시 실행한 뒤 같은 Sol review에서 current-candidate 증거로
  승격한다. 이는 정식 spec·protocol 변경 사유가 아니다.
- **ActorRef 경계 보강 후 current gate 재실행 (2026-08-08 19:14, Sol 재판정 전 checkpoint)**: 마지막 source를 포함한 net8.0
  Release unit aggregate `1,622/1,622`(`/tmp/zlink-dotnet-unit-net8-20260808-actor-invariant.log`)와
  net10.0 Release aggregate `1,622/1,622`(`/tmp/zlink-dotnet-unit-net10-20260808-actor-invariant.log`)를
  순차 실행했다. package verifier는 9개 package·standalone HTTP clean consumer·API snapshot
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`로 통과했고
  (`/tmp/zlink-dotnet-package-20260808-actor-invariant.log`, review output
  `/tmp/zlink-dotnet-http-actor-review.Lh1IvO`), TA-A4 three-process도 `to-actor-messaging e2e result=passed`를
  기록했다(`framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-191428-3187045/`). 이 checkpoint 당시에는
  freshness blocker의 실행 조건은 충족했지만, 마지막 동일 Codex Sol 재판정 전까지 HTTP 항목과 전체 집계는
  보수적으로 미종결로 유지한다.
- **동일 Codex Sol 계약 재판정 (2026-08-08 19:16, `gpt-5.6-sol`, effort `high`, 기준 commit
  `137f2858bf7fd29f58405893473be8e773725a93` + current dirty worktree, 수정 commit 없음)**: 모든 .NET
  Framework Gap·PARTIAL·public contract를 exact interface·정식 spec·production runtime·owner regression·
  package/clean-consumer·process evidence와 다시 대조했다. Actor/Spot type-owned converter의 invalid-value
  rejection과 마지막 current-candidate gate에 새 finding이 없었고, `DOTNET-HTTP-SERIALIZATION-001`과 freshness
  Low를 완료로 판정했다(`Contracts/Actors/ActorRef.cs:48-80,162-181`, `Contracts/Spots/Contracts.cs:42-127,217-236`,
  `FrameworkJsonProfileTests.cs:50-86`). 같은 candidate의 focused 27/27, net8/net10 aggregate 각각 1,622/1,622,
  net8/net10 contract test 각각 76/76(`/tmp/zlink-dotnet-contract-net8-actor.log`,
  `/tmp/zlink-dotnet-contract-net10-actor.log`),
  9개 package·standalone clean consumer·snapshot, TA-A4 three-process와 `git diff --check`가 모두 통과했다.
  계약 전체 판정은 **NOT CLEAN — High 4 / Medium 4 / Low 0**이다. 남은 High는 command 51 canonical
  integration, Config 14 Instance Spot, 전체 process Gap/PARTIAL, 양쪽 TFM single/multi-process와 6 RID
  release matrix이고, Medium은 `DOTNET-LAYER-003`, 보호 exact-interface 상태, 보호 common implementation-gap
  상태, Windows PowerShell evidence다. 새 spec/protocol 변경은 필요하지 않으며, 계약 전체가 CLEAN이 아니므로
  production-first POSDDD 2차 review는 시작하지 않는다.
- **LAYER-003 relocation 내부 key 보강 checkpoint (2026-08-08 19:25 이후)**: relocation application의 actor
  snapshot, source capture·sealed session route map을 `ZLinkActorId` key로 바꾸고, actor state restore·participant
  assembly에서 boundary string을 한 번만 변환한다(`Runtime/Spots/ZLinkSpotActivationExecution.cs:11-13,2248-2300`,
  `Runtime/Spots/ZLinkSpotRetireScheduler.cs:458-548,1250-1385`). Actor drain target map은 `RoutingId` key로,
  retire preflight의 mesh·RID 복합 key는 `(ZLinkMeshName, RoutingId)` tuple로 바꾸어 composite string 생성과
  내부 domain 혼용을 제거했다(`Runtime/Host/ZLinkActorDrainCoordinator.cs:366-397`,
  `Runtime/Spots/ZLinkSpotRetireScheduler.cs:48-101`). production net8 Release build는 warning/error 0이고,
  relocation·drain·identifier focused 220/220이 통과했다. 이 수정은 public exact interface·formal spec·wire
  protocol을 변경하지 않는다. 새 source를 포함한 net10 aggregate, package/clean-consumer와 relevant process를
  다시 실행하고 동일 Sol review를 반복하기 전까지 `DOTNET-LAYER-003`은 PARTIAL로 유지한다.
- **LAYER-003 current-candidate gate (2026-08-08 19:40 이후)**: 위 source를 포함한 net8.0·net10.0 Release 전체
  unit aggregate가 각각 `1,623/1,623`으로 통과했다(`/tmp/zlink-dotnet-unit-net8-20260808-relocation-keys.log`,
  `/tmp/zlink-dotnet-unit-net10-20260808-relocation-keys.log`). package verifier는 9개 package, standalone HTTP
  clean consumer와 API snapshot `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를 통과했다
  (`/tmp/zlink-dotnet-package-20260808-relocation-keys.log`, review output
  `/tmp/zlink-dotnet-relocation-key-package.zMrrtp`). 관련 ST-E2 three-process도 Actor owner·이전 session owner·새
  session owner evidence와 callback-before-close assertion을 포함해 통과했다
  (`framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-194019-3835927/`). 이 gate는 새 typed-key
  source가 package와 process에서 회귀하지 않았음을 확인하지만, Windows/6 RID release와 compatibility·serialization
  boundary allowlist는 아직 별도 계약 조건이다. 동일 Sol 재검토 전까지 `DOTNET-LAYER-003`은 PARTIAL로 유지한다.
- **TA-A4 current-candidate freshness blocker (2026-08-08 19:42)**: relocation-key source 이후 `ToActorMessaging`
  `TA-A4`를 다시 실행했지만 session-b가 `http://127.0.0.1:3527`에서 3초 안에 시작되지 않아 runner가
  `Timed out waiting 3s for session-b`로 실패했다(`framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-194159-3880184/`).
  이는 public contract나 protocol 변경 근거가 아니며, 이전 19:14 TA-A4 pass는 현재 source의 process evidence로
  승격하지 않는다. session-b startup/port/process log를 확인한 뒤 isolated 재실행하고, pass 전까지 HTTP
  process freshness gate를 미완료로 유지한다.
- **TA-A4 freshness 재실행 (2026-08-08 19:43)**: 직전 실패의 `session-b.stderr.log`에는 Core
  `fast_mutex.hpp:61`의 `Invalid argument`만 남았고 Framework source assertion failure는 없었다. 같은 source에서
  runner를 isolated 재실행해 `to-actor-messaging e2e result=passed`를 확인했다
  (`framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-194257-3952127/`). 따라서 TA-A4 freshness
  blocker는 재실행 pass로 해소했지만, 첫 실패는 flaky native startup evidence로 historical blocker에 남기며
  retry로 숨기지 않는다.
- **LAYER-003 raw owner registry 보강 checkpoint (2026-08-08 19:50 이후)**: 동일 Sol이 지적한 boundary 밖
  residual을 source에서 보강했다. routed-handler registry tuple의 channel key를 `ZLinkChannelName`으로,
  AutoConnect의 mesh target classification·retained member를 `RoutingId`로, pending channel weight를
  `ZLinkChannelName`으로, ClientServer discovery revision floor를 `(RoutingId, LifecycleGeneration)` tuple로
  바꿨다(`Runtime/Channels/ZLinkRouteHandlerRegistry.cs:1-48`, `Runtime/Locations/ZLinkAutoConnectReconciler.cs:38-127,434-468`,
  `Runtime/Locations/ZLinkClientServerDiscovery.cs:245-303`). public descriptor/configuration·wire/store 문자열은
  boundary에 남기고 내부 map에서만 변환한다. net8/net10 focused 72/72가 통과했고 production net8 build는
  warning/error 0이다. 이 source 뒤의 full aggregate, package/clean-consumer, process와 동일 Sol review를 다시
  실행하기 전까지 `DOTNET-LAYER-003`은 PARTIAL로 유지한다.
- **LAYER-003 current source aggregate/package/process gate (2026-08-08 20:07 이후)**: node-direct route의 빈
  sentinel을 실제 channel name과 분리한 `ZLinkRouteHandlerChannelKey`를 추가한 뒤, net8.0·net10.0 Release 전체
  aggregate가 각각 `1,623/1,623`으로 통과했다(`/tmp/zlink-dotnet-unit-net8-20260808-owner-keys-final.log`,
  `/tmp/zlink-dotnet-unit-net10-20260808-owner-keys-rerun.log`). net10의 직전 aggregate 1건 timeout은 isolated
  1/1과 full rerun pass로 재현되지 않았지만 해당 flaky 사실은 숨기지 않고 checkpoint에 남긴다. package verifier는
  9개 package·standalone HTTP clean consumer·snapshot `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를
  통과했다(`/tmp/zlink-dotnet-package-20260808-owner-keys-final.log`, review output
  `/tmp/zlink-dotnet-owner-key-package.V75i74`). ST-E2와 TA-A4 current process도 각각 callback-before-close 및
  ActorRef HTTP decode를 확인했다(`SpotActorTransfer/logs/20260808-200636-441443/`,
  `ToActorMessaging/logs/20260808-200656-443006/`). 이 evidence는 latest source에 대한 회귀를 보강하지만,
  Windows/6 RID와 보호 문서·canonical command 51 blocker가 남아 계약 전체를 CLEAN으로 올리지 않는다.
- **LAYER-003 fanout/peer residual 및 aggregate stability gate (2026-08-08 20:22 이후)**: Fanout discovery revision
  floor와 Fanout runtime publisher snapshot diff를 `(RoutingId, LifecycleGeneration)` tuple로, ClientServer Server의
  admitted peer registry를 `RoutingId` key로 바꿨다(`Runtime/Locations/ZLinkFanoutDiscovery.cs:197-257`,
  `Runtime/Locations/ZLinkFanoutRuntimeService.cs:123-131,269-272`,
  `Runtime/Channels/ZLinkClientServerServerIdentity.cs:15-182`). Relocation admission의 5초 polling은
  기존 aggregate startup 예산과 맞춘 30초 bounded helper로 조정했다(`tests/Runtime/RelocationRuntimeTests.cs:5612-5624`).
  최신 full aggregate는 net8/net10 각각 `1,623/1,623`, relocation focused는 각각 `171/171`, contract test는 각각
  `76/76`으로 통과했다(`/tmp/zlink-dotnet-unit-net8-20260808-final-typed.log`,
  `/tmp/zlink-dotnet-unit-net10-20260808-final-typed.log`, `/tmp/zlink-dotnet-contract-net8-owner-keys.log`,
  `/tmp/zlink-dotnet-contract-net10-owner-keys.log`). package/clean-consumer와 API snapshot도 통과했다
  (`/tmp/zlink-dotnet-package-20260808-final-typed.log`, review output `/tmp/zlink-dotnet-final-package.D4V2cy`).
  최신 ST-E2와 TA-A4 process도 각각 callback-before-close와 ActorRef HTTP decode를 확인했다
  (`SpotActorTransfer/logs/20260808-202150-860520/`, `ToActorMessaging/logs/20260808-202215-866694/`). 이 gate는
  LAYER-003 residual과 aggregate stability를 보강하지만, Windows/6 RID·보호 문서·canonical command 51 및 전체
  process Gap/PARTIAL이 남아 최종 계약 판정은 여전히 CLEAN이 아니다.
- **ToActorMessaging independent TA-A1 gate (2026-08-08 17:52)**: 같은 typed-identifier candidate에서
  `run_e2e.sh TA-A1`을 별도로 실행해 `to-actor-messaging e2e result=passed`를 확인했다. log는
  `framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-175249-857948/`다. 이 결과는
  RouteMesh Actor request/reply와 기본 owner handler 경로가 현재 변경으로 회귀하지 않았음을 보강하지만,
  TA-A4 ActorRef HTTP JSON blocker와 전체 ToActor process gate를 종결하지 않는다.
- **AutomaticTurnDispatch process blocker (2026-08-08)**: Linux net8.0에서
  `framework/languages/dotnet/e2e/AutomaticTurnDispatch/run_e2e.sh all`의 31개 TD-A~G client scenario와
  shutdown-wait는 통과했지만 shutdown-recovery가 exit 134로 중단됐다. log는
  `framework/languages/dotnet/e2e/AutomaticTurnDispatch/logs/20260808-160719-1542060/`이며,
  `client-shutdown-recovery.stderr.log`에서 `ShutdownAwaitProbe.cs:46,56`의 recovery node request가
  `DeadlineExceeded ... play-a-recovery ... TimedOut`으로 끝났다. 따라서 전체 automatic-turn process
  evidence는 미완료다. recovery process 재기동·route admission trace를 먼저 수집하고, timeout을 늘리거나
  scenario assertion을 줄이는 우회는 하지 않는다. public spec/protocol 변경 근거는 없다.
- **identifier boundary checkpoint (2026-08-08)**: `ZLinkRouteMeshRuntimeService`의 sequence·monitor hub
  registry를 `ZLinkMeshName` key로 바꾸고, `ZLinkStreamSessionTable`의 session·creation registry를
  `RoutingId` key로 바꿨다. 외부 string API와 `ZLinkManagedStream.SessionId` serialization boundary에서만
  변환하며 내부 dictionary는 문자열을 저장하지 않는다. MonitorHub에서 sequence를 사용할 때는
  `ZLinkMeshName` overload를 직접 호출해 typed 값이 private string 경계를 왕복하지 않는다
  (`Runtime/Host/ZLinkRouteMeshRuntimeService.cs:25-29,279-294,444-457,794`,
  `Runtime/Streams/ZLinkStreamSessionTable.cs:18-20,61-153,206-229`). `RuntimeIdentifierTests`에 두
  registry의 key type assertion을 추가했고 RouteMesh/identifier/stream-session 관련 net8.0 test 32/32가
  통과했다. 이는 LAYER-003의 내부 key residual을 줄였지만 compatibility overload와 전체 serialization
  boundary allowlist audit, release/package/process gate가 남아 최종 판정은 계속 PARTIAL이다.
- **relocation target stage identifier checkpoint (2026-08-08)**: target-side aggregate staging에서
  Actor authority generation map, staged Actor state map과 rollback set의 내부 key를 `ZLinkActorId`로
  통일했다. wire request의 Actor ID 문자열은 decode boundary에 남기고, stage admission·authority lookup·
  rollback에서만 `FromBoundary`를 수행한다(`Runtime/Host/ZLinkFrameworkRuntimeSpotRetire.cs:124-131,215-220,253-255,303-317`,
  `Runtime/Spots/ZLinkSpotRetireTransport.cs:3447-3504`). `TargetStage` authority-generation property를
  typed dictionary로 고정하는 reflection assertion도 추가했다(`RuntimeIdentifierTests.cs:167-171`). 기존
  relocation fixture를 typed key로 갱신한 뒤 RelocationRuntime focused `171/171`, RuntimeIdentifier `3/3`,
  최신 net8 aggregate `1,605/1,605`, package 9개와 standalone clean consumer를 통과했다
  (`/tmp/zlink-dotnet-relocation-focused-20260808-after-stage-actor-id.log`,
  `/tmp/zlink-dotnet-identifier-focused-20260808-after-stage-actor-id.log`,
  `/tmp/zlink-dotnet-unit-net8-20260808-after-stage-actor-id.log`,
  `/tmp/zlink-dotnet-package-20260808-after-stage-actor-id.log`). 이 checkpoint는 LAYER-003의 내부
  Actor map residual을 줄이지만 전체 compatibility/serialization allowlist와 net10·6 RID·Windows·process
  gate가 남아 항목은 PARTIAL로 유지한다.

- **local managed object registry identifier checkpoint (2026-08-08)**: `ZLinkManagedMeshNode._actors`의
  내부 key를 `ConcurrentDictionary<ZLinkActorId, ManagedActor>`로, `_spots`의 내부 key를
  `ConcurrentDictionary<ZLinkSpotId, ZLinkManagedSpot>`로, `_channels`의 내부 key를
  `Dictionary<ZLinkChannelName, uint>`로 고정했다. Actor·Spot·Channel 생성·lookup·destroy·rekey·weight
  검증 경계에서만 각 `FromBoundary`를 호출하도록 정리했다
  (`Runtime/Service/ZLinkManagedMeshNode.cs:55,73,75,424-461,1792-1884,1949-1952,1977-1981,2102-2126,
  2905-2927,2949-2954,3196-3200,3309-3313,5502-5508,8435-8444`).
  `RuntimeIdentifierTests.RuntimeRegistries_UseTheirIdentifierDomainAsTheKey`에 세 field assertion을
  추가했고 net8.0 Release identifier/weight/foundation focused test `65/65`, relocation·entry spot focused
  `308/308`, 전체 unit aggregate(`FullyQualifiedName!~Slow`) `1,605/1,605`를 이 변경 이후 다시 통과했다
  (`/tmp/zlink-dotnet-unit-net8-20260808-after-channel-key.log`). 같은 current candidate의 package verifier와
  standalone HTTP clean consumer도 9개 package, snapshot
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`로 통과했다
  (`/tmp/zlink-dotnet-package-20260808-after-channel-key.log`). 관련 ST-E2는
  `SpotActorTransfer/logs/20260808-172823-4022771/`, net10 aggregate는
  `/tmp/zlink-dotnet-unit-net10-20260808-after-channel-key.log`에서 같은 current candidate로 다시 통과했다.
  따라서 이 checkpoint의 current-candidate freshness 조건은 충족했지만, `DOTNET-LAYER-003`은 전체
  compatibility/serialization allowlist와 미실행 release·Windows·process gate가 남아 PARTIAL로 유지한다.
  SF-B1 진단에 사용한 submit/result trace는
  provider 미도착 증거를 수집한 뒤 production source에서 제거했으며, 해당 진단 변경은 계약이나 timeout
  정책에 남아 있지 않다. 이 checkpoint는 local Actor registry residual을 줄이지만 전체
  compatibility/serialization allowlist와 미실행 release·Windows·process gate가 남아
  `DOTNET-LAYER-003`은 PARTIAL로 유지한다.

- **identifier boundary residual checkpoint (2026-08-08 18:00)**: Actor membership relocation 중
  Entry Spot 전환 여부를 기록하던 `ZLinkSpotActivation._actorsLeavingForEntrySpot`의 key가
  application `ActorId` 문자열이어서 domain key가 다시 raw string으로 역류하고 있었다. 이를
  `HashSet<ZLinkActorId>`로 바꾸고 `IZLinkActor.Context.ActorId`에서만
  `ZLinkActorId.FromBoundary`를 호출하도록 정리했다(`Runtime/Spots/ZLinkSpotActivationActors.cs:7,
  561,644,675,683,756`). Bound-session submitter cache도 route의 typed `ZLinkMeshName`을 직접 key로
  사용하도록 바꾸고 coordinator의 submitter creation 경계에서만 backend lookup용 string으로
  변환한다(`Runtime/Streams/ZLinkBoundSessionService.cs:10,139-157`,
  `Runtime/Host/ZLinkActorBoundSessionCoordinator.cs:826-833`,
  `Runtime/Host/ZLinkFrameworkRuntimeActors.cs:4344-4347`). `RuntimeIdentifierTests`에 두 registry의
  key assertion을 추가했다(`RuntimeIdentifierTests.cs:122-126,164-168`). 이 변경 뒤 net8 focused
  identifier/Entry Actor gate 140/140과 BoundSession 포함 focused 45/45가 통과했으며, public spec이나
  protocol은 변경하지 않았다. 전체 boundary allowlist와 aggregate/package/process freshness는 계속
  `DOTNET-LAYER-003`의 PARTIAL 조건으로 남긴다.
- **identifier residual follow-up (2026-08-08 18:20)**: 동일 Sol 재검토에서 추가로 확인한
  `ZLinkMeshChannelSelection._plans`/`_declaredChannels`와 `ZLinkFanoutRuntimeService._automaticChannels`/
  `_states`/`_observers`의 raw `string` channel key를 `ZLinkChannelName`으로 전환했다
  (`Runtime/Service/ZLinkMeshChannelSelection.cs:16-18`,
  `Runtime/Locations/ZLinkFanoutRuntimeService.cs:11-15`). 문자열 public/runtime 호출은 각 메서드 시작에서
  한 번만 `FromBoundary`하고, public snapshot/event에는 `.Value`를 사용한다. `RuntimeIdentifierTests`에
  다섯 registry key assertion을 추가했고 net8.0 Release `RuntimeIdentifierTests`, `WeightContractTests`,
  `FanoutAutomaticDiscoveryTests` 23/23이 통과했다. 이 변경은 정식 spec·protocol을 바꾸지 않았지만,
  current source의 net8/net10 aggregate, package/clean-consumer와 relevant process evidence를 다시
  실행하기 전까지 `DOTNET-LAYER-003`은 PARTIAL로 유지한다.
- **submitter lifecycle race checkpoint (2026-08-08 18:35)**: `ZLinkAsyncSubmitter.Drain`이 ready credit를
  관찰한 뒤 `DisposeAsync`가 admission을 닫는 경쟁에서 `TrySubmitNow`의 `ObjectDisposedException`이
  native ready callback 밖으로 전파되지 않도록 pending item의 terminal failure로 변환하고 dequeue하도록
  수정했다(`Runtime/Messaging/ZLinkAsyncSubmitter.cs:603-642`). 기존 race regression에 dispose 뒤 ready
  callback 재호출 assertion을 추가했다(`tests/Zlink.Framework.UnitTests/Runtime/ZLinkAsyncSubmitterTests.cs:585-586`).
  net8.0 focused 3/3과 net10.0 focused 1/1이 통과했으며, production source는 public spec이나 protocol을
  바꾸지 않았다. 전체 net8/net10 aggregate와 package/process freshness를 다시 실행하고 동일 Sol review로
  race가 닫혔는지 확인하기 전까지 `DOTNET-SUBMITTER-DISPOSE-RACE-001`은 미완료로 유지한다.
- **current-candidate replacement freshness gate (2026-08-08 18:27)**: fanout/channel typed-key와 submitter
  lifecycle race 수정이 포함된 current binary에서 `SpotActorTransfer ST-E2`를 다시 실행했다. log는
  `framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-182644-2031944/`이며 Actor owner,
  이전 session owner, 새 session owner의 role evidence와 client assertion
  `ST-E2 callback guidance received before close`를 확인했다. 동일 Sol 재검토에서도 replacement process
  freshness Medium 종결을 확인했다. TA-A4 HTTP serialization High, canonical protocol, Windows·RID·전체
  process gate는 여전히 남아 있으므로 전체 contract를 CLEAN으로 올리지 않는다.
- **current-candidate aggregate/package refresh (2026-08-08 18:40)**: submitter drain race 보정과 fanout/channel
  typed-key를 포함한 동일 source에서 net8.0 Release aggregate `1,605/1,605`
  (`/tmp/zlink-dotnet-unit-net8-20260808-after-fanout-submit-race.log`)와 net10.0 Release aggregate
  `1,605/1,605`(`/tmp/zlink-dotnet-unit-net10-20260808-after-fanout-submit-race.log`)를 순차 실행했다.
  package verifier는 9개 package와 standalone HTTP clean consumer, API snapshot
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`로 통과했다
  (`/tmp/zlink-dotnet-package-20260808-after-fanout-submit-race.log`, review directory
  `/tmp/zlink-dotnet-review-fanout-submit-race.IJDMNU`). 이 gate들은 submitter race의 첫 관찰 이후
  production 수정이 aggregate/package를 깨뜨리지 않았음을 확인했다. TA-A4 serialization, canonical
  protocol, Windows·RID·전체 process gate는 여전히 별도 종결 조건이다.
- **command 51 cross-language protocol 재검토 (2026-08-08)**: 사용자의 요청에 따라 .NET 구현을 Java/Kotlin,
  Node.js, C++ 구현과 공통 계약에 대조했다. Java의 `ZLinkServiceM6BWireCodec`는 이미
  `COMMAND_BOUND_SESSION_REPLACED`를 encode/decode하고 내부 mesh node가 수신·재시도·handler를 연결한다
  (`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6BWireCodec.java:350-392`,
  `.../binding/ZLinkJavaRawMeshNode.java:2629-2667,4061-4062,5549-5574`). 현재 worktree의 Node.js와 C++도 같은
  record body와 strict trailing-input 검증을 구현한다
  (`framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-wire-codec.ts:637-651`,
  `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:805-858`). 따라서 command 51은
  .NET만의 임의 확장이 아니며 다른 언어가 기존 `boundSessionBind(38)` 또는 application packet으로 대체할 수 있는
  동작도 아니다.

  반면 기준 commit `137f2858bf`의 canonical `framework/runtime/protocol/service-wire-v1.schema.json`은
  `51..255`를 reserved로 두고 command 51 정의가 없다(`git show 137f2858bf:framework/runtime/protocol/service-wire-v1.schema.json:4900-4903`).
  현재 `main` HEAD `cca88f92ac`도 schema에서는 같은 예약을 유지하고, 기준 commit의 공통 session-actor spec에도
  command 51 계약이 없다. 반면 현재 worktree의 schema·common spec·generator·validator와 Node/C++ generated assets가
  command 51을 추가한 상태다. Worktree schema의 command 51 body와 semantic
  constraints는 `framework/runtime/protocol/service-wire-v1.schema.json:4741-4764`, 새 reserved 범위는
  `:4940-4942`에 있다. 즉 실제로 필요한 변경은 .NET public API 변경이 아니라 공통 service-wire canonical source에
  command 51을 정식 등록하고 golden·generated asset을 재생성하는 protocol/spec amendment다. 이 변경 없이 .NET이
  숫자 51을 직접 보내면 schema/generator 정본과 어긋나고, 기존 command를 재사용하면 공통 계약과 다른 wire 의미가
  되어 해결할 수 없다. 사용자가 보호한 스펙 변경 금지 조건에 따라 해당 변경은 실행하지 않고
  `DOTNET-CONTRACT-INTEGRATION-001` blocker로 유지한다. 새 스펙 변경은 발생하지 않았다.
- **LAYER-003 residual 보강 (2026-08-08)**: 동일 Sol이 지적한 내부 residual을 spec-neutral하게 수정했다.
  Automatic Fanout subscriber의 channel field를 `ZLinkChannelName`으로, publisher connection registry를
  `(RoutingId PublisherRid, ulong LifecycleGeneration)` tuple key로 바꾸고 boundary에서만 `.Value`를 사용한다
  (`Runtime/Channels/ZLinkAutomaticFanoutSubscriberRuntime.cs:21-79,159-170,285-289`). ClientServer server identity의
  channel property와 peer registry를 각각 `ZLinkChannelName`과 `RoutingId`로 고정하고, automatic successor lookup은
  `RoutingId` key를 사용한다(`Runtime/Channels/ZLinkClientServerServerIdentity.cs:15-31,148-182,263-276`,
  `Runtime/Channels/ZLinkClientServerClientRuntime.cs:103-130,682-686`). Channel value type을 도입한 뒤 control
  receiver의 string 비교 경계에서 `.Value`를 명시해 정상 admission이 reject되지 않도록 보정했다
  (`Runtime/Channels/ZLinkChannelReceiveLoop.cs:198-207`). net8 `ClientServerChannelRuntimeTests` 30/30,
  net8 identifier/fanout 9/9, net10 동일 범위 39/39가 통과했고 production build는 0 warning/0 error다.
  전체 aggregate와 package/process freshness 및 동일 Sol 재검토 전까지 LAYER-003은 PARTIAL로 유지한다.
- **command 51 동일 Sol 재검토 및 현재 aggregate (2026-08-08)**: 동일 `gpt-5.6-sol`, effort `high`가
  기준 commit `137f2858bf7fd29f58405893473be8e773725a93`와 current dirty worktree를 기준으로 command 51
  blocker를 다시 검토했다. 수정 commit은 없고 보호 spec·canonical protocol도 변경하지 않았다. 기준 schema는
  command `51..255`를 reserved로 두며(`framework/runtime/protocol/service-wire-v1.schema.json@137f:4900-4903`),
  기준 common session-actor spec은 이전 cleanup ACK 이후 bind terminal을 반환하는 동작이다. 반대로 current
  candidate는 새 binding 선확정·ACK 비대기 terminal·command 51·100 ms non-blocking close를 요구한다.
  Java의 `ZLinkServiceM6BWireCodec`/`ZLinkStreamRuntime`, Kotlin의 Java runtime bridge, current Node/C++의 동일
  codec·exact retired fence 구현을 대조한 결과, command 51은 .NET 전용 확장이 아니지만 clean `main` 계약을
  대신하지도 않는다. `boundSessionBind(38)` 재사용은 no-ACK terminal과 충돌하고 application/private packet 우회는
  공통 wire parity와 exact owner fence를 잃는다. 따라서 결론은 **High 1건: common spec·service-wire schema·golden·
  generated asset amendment 승인 필요**, spec-neutral 해법 없음, 사용자 지시에 따라 `DOTNET-CONTRACT-INTEGRATION-001`
  및 `DOTNET-SESS-REPLACE-001`을 BLOCKED로 유지한다.

  이 residual source를 포함한 current Release aggregate는 net8.0 `1,623/1,623`
  (`/tmp/zlink-dotnet-unit-net8-20260808-current-keys.log`), net10.0 `1,623/1,623`
  (`/tmp/zlink-dotnet-unit-net10-20260808-current-keys.log`)으로 통과했다. 이는 runtime/unit freshness만 갱신한
  것이며 package/clean-consumer·관련 process와 Windows/6-RID gate는 별도 실행 조건이다. current source를
  명시한 contract test도 net8.0·net10.0에서 각각 `76/76`으로 통과했다
  (`/tmp/zlink-dotnet-contract-net8-20260808-current-keys.log`, `/tmp/zlink-dotnet-contract-net10-20260808-current-keys.log`).
- **최신 current-candidate 증거 갱신 (2026-08-08 20:54)**: 위 typed identifier·fanout·peer residual을 포함한
  동일 source로 net8.0과 net10.0 Release aggregate를 각각 `1,623/1,623`으로 다시 통과시켰다
  (`/tmp/zlink-dotnet-unit-net8-20260808-current-keys.log`, `/tmp/zlink-dotnet-unit-net10-20260808-current-keys.log`).
  `framework/languages/dotnet/scripts/verify_packaged_contract.sh` strict 모드도 9개 NuGet package,
  standalone HTTP clean consumer와 public API snapshot
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를 통과했다
  (`/tmp/zlink-dotnet-package-20260808-current-keys-strict.log`). 이 과정에서 typed internal signature를
  반영하도록 package XML contract snapshot의 hash만 `fd8c6c0604ed040435fb7dccfd9c248e94a266b2b17a632d0976e059c780fb5d`로
  갱신했으며 public spec·service-wire schema·golden은 수정하지 않았다.
  최신 `SpotActorTransfer ST-E2`도 Actor owner·이전 session owner·새 session owner process와
  `callback guidance received before close` assertion으로 통과했고
  (`framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-205326-1570824/`),
  `ToActorMessaging TA-A4`도 `to-actor-messaging e2e result=passed`와 ActorRef HTTP decode evidence로 통과했다
  (`framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-205350-1590459/`).
  이 네 gate는 current source freshness를 보강하지만 Windows/6-RID, 양쪽 TFM single/multi-process, 전체
  Gap/PARTIAL process와 canonical command 51 통합 blocker를 닫지 않는다.
- **동일 Codex Sol 최신 전체 계약 재검토 (2026-08-08 20:56, typed owner-key 수정 전 historical)**: 동일 `gpt-5.6-sol`, effort `high`가 모든 .NET
  Framework Gap·PARTIAL·public contract를 exact interface·정식 spec·production runtime·owner-layer regression·
  package/clean-consumer·process evidence와 다시 대조했다. 기준은 초기 후보 `137f2858bf7fd29f58405893473be8e773725a93`,
  현재 `main` HEAD `cca88f92ac06d910d65bb2a1202ad1b970e44492`와 current dirty worktree이며, reviewer의 수정 commit·파일
  수정은 없다. 최신 aggregate/package/ST-E2/TA-A4 결과에는 새 correctness finding이 없었다.
  다만 `DOTNET-LAYER-003`에는 codec·Store·diagnostic boundary가 아닌 production owner key가 남아 있다.
  AutoConnect owner의 `string MeshName` 저장(`Runtime/Locations/ZLinkLocationAutoConnectHost.cs:30`)과 remote request
  admission의 `(string ActorId, string BindingToken)` key/parameter(`Runtime/Host/ZLinkBoundedRemoteRequestAdmission.cs:7`)는
  typed boundary allowlist를 아직 충족하지 않는다. 따라서 전체 판정은 **NOT CLEAN — High 4 / Medium 4 / Low 0**이다.
  High는 `DOTNET-CONTRACT-INTEGRATION-001`/`DOTNET-SESS-REPLACE-001`의 command 51 formal amendment,
  Config 14 Instance Spot, 전체 process Gap/PARTIAL, 양쪽 TFM single/multi-process와 6 RID release matrix다.
  Medium은 `DOTNET-LAYER-003`, 보호된 .NET exact-interface 상태, 보호된 common implementation-gap 상태와
  Windows PowerShell evidence다. Low consistency는 다음 근거를 보강해 해소했다: 현재 HEAD에서 golden은 tracked지만
  schema는 여전히 51을 reserved로 두고, 최신 ST-E2 path는 `SpotActorTransfer/logs/20260808-205326-1570824/`다.
  이 결과로 계약 단계가 CLEAN이 아니므로 production-first POSDDD 2차 review는 시작하지 않는다.

- **LAYER-003 typed owner residual 후속 게이트 (2026-08-08 21:24)**: 위 재검토가 지적한 두 production raw owner key를
  public contract나 protocol을 바꾸지 않고 domain value boundary로 전환했다. `ZLinkLocationAutoConnectHost`의
  `MeshName` 저장·local loop/reconciler key는 `ZLinkMeshName`으로, `ZLinkBoundedRemoteRequestAdmission`의
  `(string ActorId, string BindingToken)` dictionary와 호출 경계는 `ZLinkSessionBindingKey`로 고정했다
  (`framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkLocationAutoConnectHost.cs`,
  `Runtime/Locations/ZLinkAutoConnectPlanner.cs`, `Runtime/Locations/ZLinkAutoConnectLoop.cs`,
  `Runtime/Locations/ZLinkAutoConnectReconciler.cs`, `Runtime/Host/ZLinkBoundedRemoteRequestAdmission.cs`,
  `Runtime/Actors/ZLinkActorBoundSessionCoordinator.cs`). 문자열은 repository·descriptor·wire boundary에서만 변환한다.
  production net8 build는 warning/error `0/0`, owner focused regression은 net8/net10 각각 `90/90`으로 통과했고
  (`/tmp/zlink-dotnet-build-net8-typed-owner-residual.log`,
  `/tmp/zlink-dotnet-focused-net8-typed-owner-residual.log`,
  `/tmp/zlink-dotnet-focused-net10-typed-owner-residual.log`), 최신 전체 aggregate도 net8/net10 각각
  `1,622/1,622`로 통과했다 (`/tmp/zlink-dotnet-unit-net8-20260808-typed-owner-residual.log`,
  `/tmp/zlink-dotnet-unit-net10-20260808-typed-owner-residual.log`).
  최신 package verifier는 9개 package, standalone HTTP clean consumer와 public API snapshot
  `d73b00de03585fcbc14e220ce7ae5117b8b3b58f8751f6b05ddff02869453144`를 통과했고, package XML snapshot hash는
  `19288e149fadd087791bbc38f730d7a906fd3d66ac3c36f7aef50207d872c42b`로 갱신했다
  (`/tmp/zlink-dotnet-package-typed-owner-residual-strict.log`,
  `/tmp/zlink-dotnet-package-typed-owner-residual-generate.log`).
  동일 source의 ST-E2는 Actor owner·이전 session owner·새 session owner process에서 callback guidance가 close보다
  먼저 도착한 뒤 통과했고 (`framework/languages/dotnet/e2e/SpotActorTransfer/logs/20260808-212305-2229477/`,
  `/tmp/zlink-dotnet-st-e2-typed-owner-residual.log`), TA-A4도 `to-actor-messaging e2e result=passed`로 통과했다
  (`framework/languages/dotnet/e2e/ToActorMessaging/logs/20260808-212328-2239176/`,
  `/tmp/zlink-dotnet-ta-a4-typed-owner-residual.log`). 이전 Sol finding의 owner-key 부분은 수정됐으며, 아래 최신
  동일 Codex Sol 최종 재검토에서 해당 residual 종결을 확인했다. canonical spec/protocol은 수정하지 않았다.
- **E2E 후속 실행 보류 (2026-08-08)**: 사용자가 이번 작업에서는 E2E를 실행하지 않고 후속 작업에서 다시 진행하도록
  지시했다. 따라서 이미 최신 소스로 통과한 ST-E2/TA-A4 결과는 참고 증거로 남기되, Config 14 InstanceSpot·전체
  Gap/PARTIAL process suite·Windows PowerShell 및 6-RID process gate는 `deferred`로 유지한다. 이 보류는 완료를
  의미하지 않으며, 최종 종료 판정 전에 해당 process gate를 재개해야 한다.
- **동일 Codex Sol 최종 계약 재검토 (2026-08-08, 최신 typed owner-key source)**: 동일 `gpt-5.6-sol`, effort `high`가
  기준 commit `137f2858bf7fd29f58405893473be8e773725a93`, 현재 `main` HEAD
  `cca88f92ac06d910d65bb2a1202ad1b970e44492`와 dirty worktree를 대상으로 모든 .NET Framework Gap·PARTIAL·public
  contract를 exact interface·정식 spec·production runtime·owner-layer regression·package/clean-consumer·process
  evidence와 대조했다. reviewer의 수정 commit과 파일 수정은 없다. `ZLinkMeshName`과
  `ZLinkSessionBindingKey` 전환으로 `DOTNET-LAYER-003`의 마지막 production owner-key residual은 종결됐다
  (`Runtime/Locations/ZLinkLocationAutoConnectHost.cs:31`, `Runtime/Host/ZLinkBoundedRemoteRequestAdmission.cs:9`).
  configuration·Store/wire serialization·endpoint·packet/handler name·metadata·opaque binding token의 문자열은
  허용된 boundary로 재분류됐으며 새 production identifier-owner residual은 확인되지 않았다.

  `OnActorBindingReplacedAsync(...)`는 .NET만의 새 blocker가 아니다. `.NET`의 default interface method
  (`Contracts/Streams/IZLinkSession.cs:17`), Java의 default method (`systems/zlink/framework/streams/ZLinkSession.java:22`),
  Kotlin의 suspending bridge (`zlink-framework-kotlin/.../ZLinkSuspendingHandlers.kt:325`), Node의 optional callback
  (`packages/framework/src/contracts/Streams/IZLinkSession.ts:6`), C++의 default virtual callback
  (`framework/include/zlink/framework/contracts/streams/stream.hpp:252`)이 같은 application callback surface를 제공한다.
  따라서 남은 blocker는 public interface 변경이 아니라 공통 wire allocation이다. clean `main`의 service-wire schema는
  command `51..255`를 reserved로 두고, dirty candidate만 `boundSessionReplaced(51)`와 exact retired fence를 등록한다.
  다른 언어의 codec 구현은 이 동작이 .NET 전용이 아님을 확인하지만, clean canonical schema·common spec·golden·generated
  asset의 승인된 통합을 대신하지 않는다. 이 공통 amendment가 없으면 command 51 전송은 schema 정본과 충돌하고,
  기존 command 38 재사용이나 application packet 우회는 계약 의미를 보존하지 못한다. 따라서
  `DOTNET-CONTRACT-INTEGRATION-001`/`DOTNET-SESS-REPLACE-001`은 **High blocker**로 유지하며 spec/protocol은 수정하지 않는다.

  최신 source 증거는 production build warning/error `0/0`, focused owner net8/net10 각 `90/90`, full aggregate net8/net10
  각 `1,622/1,622`, 9개 package·standalone HTTP clean consumer, API snapshot `d73b00de...`, ST-E2와 TA-A4 process
  pass다. 최종 계약 판정은 **NOT CLEAN — High 4 / Medium 3 / Low 0**이다. High는 canonical command 51 amendment,
  Config 14 InstanceSpot, 전체 Gap/PARTIAL process suite, 양쪽 TFM single/multi-process와 6 RID release matrix다.
  Medium은 보호된 .NET exact-interface 상태 행, 보호된 common implementation-gap 상태 행, Windows PowerShell
  evidence다. 문서 갱신 뒤 service-wire validator·asset check·decoder fixture도 각각 통과했으며
  (`/tmp/zlink-wire-validate-doc-refresh.log`, `/tmp/zlink-wire-generate-doc-refresh.log`,
  `/tmp/zlink-wire-fixtures-doc-refresh.log`), Framework 문서 tab/link 검사도 통과했다
  (`/tmp/zlink-doc-tabs-framework-final.log`, `/tmp/zlink-doc-links-framework-plan-final.log`). 동일 Sol review에서 새
  correctness finding은 없었고 계약 단계가 CLEAN이 아니므로 production-first POSDDD 2차 review는 시작하지 않는다.

## 6. 구현 순서

Session replacement의 callback failure 분류, owner lifecycle regression, exact same-session lifecycle과 atomic
closing admission은 현재 candidate에서 수정했다. 실제 owner transport-to-runtime exact fence regression도
net8/net10 focused gate에서 완료했다. 최신 current source의 net8/net10 aggregate `1,622/1,622`, strict package
verifier 9개 package·standalone HTTP clean consumer, ST-E2와 TA-A4 three-process evidence를 갱신했고,
send-ready·Dispose race와 HTTP object-reference serialization도 production 수정·aggregate·process 회귀로
종결했다. relocation·AutoConnect·remote admission의 production owner key도 `ZLinkMeshName`과
`ZLinkSessionBindingKey` typed value로 보강했으며, 위 focused/aggregate/package/process gate를 통과했다. 다만
`DOTNET-LAYER-003` production owner-key residual은 최신 동일 Sol 재검토에서 종결됐다.
command 51 canonical schema amendment가 clean `main`에 통합되지 않았고 Config 14와 그 밖의 7개 process suite,
양쪽 TFM single/multi-process와 6개 RID full release gate, Windows evidence가 남아 있다. 보호 exact interface와
공통 implementation-gap의 상태 행도 현재 runtime과 다르지만 사용자 승인 없이 변경하지 않았다. 최신 동일
Codex Sol 계약 재검토는 **High 4건·Medium 3건·Low 0건, NOT CLEAN**으로 확정했으며, 계약 전체가 CLEAN이 아니므로
POSD/DDD 2차 review와 최종 완료 판정을 시작할 수 없다. 이 문서의 현재 전체 판정은 **미완료**다.
