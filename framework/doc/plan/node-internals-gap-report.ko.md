---
title: "Node Framework 스펙 구현 Gap 리포트"
---

# Node Framework 스펙 구현 Gap 리포트

- **작성일**: 2026-08-07
- **공개 계약 기준**: 승인된 DEC-01–17의 결과가 반영된 `framework/doc/framework/common/spec/`와
  Node server exact interface
- **내부 구조 기준**: `framework/doc/framework/common/internals/` 01–12
- **구현 기준**: `framework/languages/node/packages/framework/src/`, 생성된
  `packages/framework/dist/` declaration과 `framework/languages/node/test/` (전체 audit `425b9c2a8272`; 복구·머지 뒤 현재 `main` `e2119caeda`까지 production source 변경 없음)
- **방법**: public exact surface, runtime 의미·error, codec·protocol, lifecycle·HWM·relocation,
  package·test·process E2E 증거를 분리해 비교했다. Internals 12개 문서의 **Decision** /
  **Result To Confirm** 항목도 현재 정식 spec과 충돌하지 않는 범위에서 다시 판정했다.
- **증거 경로 표기**: `node/` = `framework/languages/node/packages/framework/src/` 축약. 형제 패키지는 `packages/…`로 표기.

Public API와 사용자에게 보이는 동작은 정식 spec과 exact interface만 계약 근거로 사용한다. Internals는
그 계약을 구현하는 상태 표현, component 책임과 불변 조건의 차이를 판정하는 기준이며 공개 동작을
추가하거나 바꾸지 않는다. Source에서 확인한 gap과 package·process 검증 증거 부족도 분리한다.

> **기존 기록과의 관계**: Public spec에 있던 구현 진행 기록은 삭제됐다.
> 이 보고서가 Node open gap의 작업 기록을 소유하며, 승인된 결정을 반영한 정식 spec과
> exact interface를 계약 기준으로 삼는다. 이전에 종결한 `NODE-ROUTE-001`,
> `NODE-RELOC-001`, `NODE-SAMPLE-001`은 코드와 일치함을 확인했다.

---

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

## 1. 집계

| 문서·범위 | GAP | PARTIAL | 비고 |
|---|---:|---:|---|
| 01-layering | 1 | 2 | shutdown-relocation 경합 역전, 식별자 내부 표현 |
| 02-serialization | 2 | 2 | 재진입 허용이 핵심 |
| 03-progress-isolation | 1 | 3 | Request 무응답 폐기 |
| 04-completion | 3 | 1 | 완료 테이블 무한 누적 |
| 05-relocation-continuity | 3 | 1 | seal 창 메시지 손실 |
| 06-routing-and-cache | 2 | 3 | 선택기 상태 리셋 |
| 07-dispatch-loop | 1 | 3 | cross-owner HOL 블로킹 |
| 08-object-lifecycle | 3 | 2 | idle sweep, 일반 message generation 거부, Ready owner loss |
| 09-session-binding | 3 | 1 | 하트비트 레인, 스왑 핸드셰이크 |
| 10-liveness-and-state | 0 | 2 | 대체로 충실 |
| 11-message-ownership | 3 | 3 | content-type 미사용, 접근자 복사 |
| 12-service-wire-protocol | 4 | 6 | RouteMesh 금지 상한 잔존, STREAM EMSGSIZE 진단, json-v1 |
| **internals 소계** | **26** | **29** | STREAM 초과 진단과 Ready owner loss gap을 반영 |
| 결정 반영 후 public contract·exact interface(기존 항목과 중복 제외) | 7 | 0 | DEC-01·02·04·06·08·09·17 |
| **전체 unique gap** | **33** | **29** | `NODE-WIRE-002`(DEC-05)는 internals 26건에만 계산 |

### 1.1 결정 반영 후 추가된 public contract gap

| ID | 분류 | spec과 현재 구현의 차이 |
|---|---|---|
| `NODE-CONTRACT-DIAG-001` | GAP·중 | Exact interface는 diagnostics level을 `off/errors/normal/detailed` 네 값으로 고정하지만 source·생성 declaration은 `Off/ErrorsOnly/KeyTransitions/Verbose`를 export한다. 문자열 값도 `errorsOnly/keyTransitions/verbose`로 다르다. 증거: Node exact interface `interfaces/01-foundation-configuration.ko.md:81-83`, `node/contracts/Dispatch/ZLinkDispatchOptions.ts:186-199`, `packages/framework/dist/contracts/Dispatch/ZLinkDispatchOptions.d.ts` |
| `NODE-CONTRACT-DIAG-002` | GAP·상 | Exact interface가 제거한 public observer, runtime error sink, raw event DTO, trace file·label option을 source·package가 아직 export한다. Runtime도 provider integration 대신 observer queue와 `console`/동기 file append를 사용한다. DEC-01의 순서에 따라 다섯 언어 logger provider process E2E를 먼저 통과한 뒤 같은 변경에서 legacy public API를 제거해야 한다. 증거: Node exact interface `interfaces/01-foundation-configuration.ko.md:451-456`, `node/contracts/Dispatch/ZLinkDispatchOptions.ts:14-29, 36-161, 169-177`, `node/contracts/RouteMesh/Contracts.ts:56-66`, `node/runtime/diagnostics/message-flow.ts:139-169, 180-280, 233-244` |
| `NODE-CONTRACT-LOC-001` | GAP·중 | Exact interface는 Actor·Spot ID 개별 조회와 `creating/ready/unavailable` 상태를 요구한다. Source·package는 paged list만 있고 `unavailable`을 타입에서 제외한다. List도 live owner를 확인하지 못한 row를 제거하므로 commit 후 owner 사용 불가를 `unavailable` entry로 반환하지 못한다. 증거: Node exact interface `interfaces/03-location-observability.ko.md:55-100`, `node/contracts/Locations/RuntimeQuery.ts:13-47`, `node/runtime/locations/runtime.ts:771-821`, generated declaration `RuntimeQuery.d.ts:3-22` |
| `NODE-CONTRACT-STREAM-001` | GAP·상 | Exact interface의 `ZLinkSessionSendCall.timeout(timeoutMs)`이 source·package에 없고 runtime은 socket timeout보다 빠른 call별 admission deadline을 받지 못한다. `AbortSignal`은 Node 자체 cancellation이며 이 timeout을 대체하지 않는다. 증거: Node exact interface `interfaces/02-channel-messaging.ko.md:423-436`, `node/contracts/Streams/IZLinkSession.ts:41-49`, `node/runtime/streams/session-calls.ts:97-142`, generated declaration `IZLinkSession.d.ts:34-42` |
| `NODE-CONTRACT-CLIENTSERVER-001` | GAP·중 | Server role만 등록한 process에서 ClientServer send/request를 시작하면 `NotConfigured` Framework error로 끝나야 한다. 현재 outbound 경로는 Client role 부재를 `ZLinkConfigurationException` 예외로 던져 public error kind를 제공하지 않는다. 증거: common spec `09-client-server-channel.ko.md:63-66`, `node/runtime/channels/channel-runtime-manager.ts:242-260`, `node/runtime/channels/channel-socket-registry.ts:611-616`, `node/runtime/channels/channel-outbound-operations.ts:86-100, 143-161` |
| `NODE-CONTRACT-TIMER-001` | GAP·중 | 세 overrun policy의 runtime tick 순서는 스펙 방향과 일치하지만 `CatchUpBounded.maxCatchUpTicks`를 정수 `1..2_147_483_647`로 startup에서 검증하지 않는다. 현재 validator는 `<= 0`만 거부하므로 소수·`Infinity`·`INT_MAX` 초과를 수락한다. 증거: common spec `05-async-execution-policy.ko.md:453-475`, Node exact interface `interfaces/06-stream-worker.ko.md:162-166`, `node/contracts/Configuration/TimerRegistrationValidator.ts:7-29`, policy runtime `node/runtime/spots/spot-timer.ts:286-362` |
| `NODE-CONTRACT-RELOC-001` | GAP·상 | Participant state가 64 MiB를 초과하면 `Blocked/StateIncompatible`로 끝나야 하지만 capture는 일반 `Error`를 던지고 host는 이를 `Blocked/RelocationFailed`로 축약한다. 크기 상한 자체는 있지만 application-visible reason이 틀리다. 증거: DEC-17, common spec `28-graceful-drain-handoff.ko.md` participant limit, `node/runtime/host/service-relocation-host-runtime.ts:2811-2828`, `node/runtime/host/index.ts:873-883` |

`NODE-WIRE-002`는 DEC-05가 `framework-json-v1`을 public codec contract로 옮긴 후에도 유효하다.
단순한 internals 구현 차이가 아니라 다섯 언어 payload 호환성을 깨뜨리는 public contract gap으로
심각도 근거를 교정했다.

### 1.2 결정에서 명시적으로 제외한 항목

- DEC-03은 비동기 I/O 대기가 CPU worker capacity를 점유하지 않는다는 runtime 결과를
  요구할 뿐 public I/O queue·thread 옵션을 요구하지 않는다. 새 option 부재를 gap으로 분류하지
  않았고 CPU 포화 process test를 증거 gap으로 남겼다.
- DEC-07에 따라 STREAM 인증은 application session callback 책임이다. Framework auth state·gate가
  없다는 이유로 public gap을 만들지 않았다.
- DEC-10은 target 선택과 host-scoped Relocate를 Framework 책임으로 유지한다. Target hint와
  Actor-scoped relocation public API를 요구하지 않았다.
- DEC-11은 기존 `notifyDisconnected(...)`의 logical disconnect를 사용한다. 별도 `Unbind`
  API를 요구하지 않았다.
- DEC-12는 public packet sequence와 inbound observer를 제공하지 않는다. Correlation ID,
  packet name, message kind와 application marker로 E2E를 판정한다.
- DEC-13은 `messageFollow` suppression 방법을 구현 선택으로 남겼다. 중복 억제
  marker·수명이 없다는 이유만으로 gap을 만들지 않았다.

### 1.3 구현 checkpoint

아래 항목은 owner-layer source와 회귀 test까지 반영했지만 package·process gate를 아직 실행하지 않았다.
따라서 집계에서는 GAP 또는 PARTIAL을 유지한다. Package와 실제 process evidence가 통과한 뒤에만 종결한다.

| ID | Source checkpoint | 현재 evidence | 남은 evidence |
|---|---|---|---|
| `NODE-CONTRACT-TIMER-001` | `160e7f2c23` | `npm run build`, `startup-validation.test.js` 23/23 PASS. `CatchUpBounded`에서만 정수 `1..2_147_483_647`을 허용하며 경계값과 소수·무한대·상한 초과를 검증한다. | packed package·clean consumer에서 같은 startup rejection 확인 |
| `NODE-CONTRACT-CLIENTSERVER-001` | `d8ef6fc15c` | Public `ZLinkChannelClient`와 runtime-manager 회귀 test PASS. Server role만 있는 Channel의 send/request가 `NotConfigured`로 끝난다. | Server-only 실제 host process에서 local handler 미실행과 send/request terminal 확인 |
| `NODE-CONTRACT-RELOC-001` | `b4c79be5c1` | `npm run build`와 focused relocation regression PASS. 64 MiB 초과를 typed internal failure로 보존하고 host 결과를 `Blocked/StateIncompatible`로 분류한다. | 64 MiB 경계·64 MiB+1 byte Store/process E2E와 source authority 유지 확인 |
| `NODE-ROUTE-002`, `NODE-ROUTE-003`, `NODE-ROUTE-005` | `69c7c7614c` | `npm run build`, `npm run typecheck`, `service-selection.test.js` 5/5 PASS. ClientServer도 공통 smooth weighted selector를 사용하고 누적값을 보존한다. Cycle 탐색은 4,096 step 또는 10 ms 뒤 exact direct 계산으로 전환하며 두 topology가 ordinal RID 비교를 사용한다. | packed package 회귀, 실제 multi-process 분포와 크로스 언어 `B,A,B,B` 수렴 확인 |
| `NODE-OBS-001` | `d51de2ac08` | `npm run build`와 focused `topology-runtime-projection.test.js` PASS. Terminal 보관 자리가 찼을 때 가장 오래된 status를 버리고 최신 status와 관찰자별 `discardedTerminalCount`를 전달한다. | 느린 관찰자 process/benchmark와 aggregate runtime gate 확인 |
| `NODE-STREAM-SIZE-001` | `80ce3593e7` | `npm run build`와 direct·segmented frame focused regression PASS. 초과를 typed `ZLinkStreamMessageSizeError`로 분류하고 diagnostics sink에 `code=EMSGSIZE`를 기록한 뒤 handler 전달 없이 peer를 disconnect한다. | packed package와 raw client/server process E2E에서 trace·disconnect 확인 |
| `NODE-LIFE-001` | `ed8b70a31e` | `npm run build`, idle scan·eviction focused regression 3/3 PASS. Activation registry가 scan cursor를 소유하고 주기마다 최대 64개만 반환하며 다음 주기에 이어서 검사한다. | 대규모 Instance Spot process 부하에서 event-loop 진행과 전체 후보 순회 확인 |
| `NODE-LIFE-003` | `8ced7e2d5f` | `npm run build`, `npm run verify:m6b-runtime` 85/85 PASS. 일반 Actor send/request는 `ActorId`로 현재 incarnation을 resolve하고, session binding과 exact control은 기존 generation fence를 유지한다. | packed package와 실제 이전 generation `ActorRef` process E2E에서 현재 Actor handler 도달 확인 |
| `NODE-WIRE-008` | `6dd490f8f8` | `npm run build`, `location-runtime.test.js` 40/40 PASS. Actor Message Follow fence가 일치할 때 direct authority cache와 legacy Actor location cache를 함께 무효화하고, 이전 fence이면 두 cache를 모두 보존한다. | packed package와 Actor relocation process E2E에서 stale route 재오염이 없는지 확인 |
| `NODE-FOLLOW-001`, `NODE-FOLLOW-002` | `256869dee0` | `npm run build`, `actor-handoff.test.js` 22/22, `npm run verify:m6b-runtime` 86/86 PASS. Message Follow queue 한도는 `CapacityExceeded`, Actor generation 불일치는 `InvalidOperation`으로 분류하고 Spot request wire reply도 같은 typed capacity failure를 보존한다. | packed package와 Actor·Spot relocation process E2E에서 caller terminal과 one-way 진단 확인 |
| `NODE-WIRE-005` | `6947cc8adb` | `npm run build`, `location-runtime.test.js` 41/41 PASS. Shared `authority-key-v1.json` fixture 전 항목을 소비하고 decoder가 1..255-byte identity, leading-zero 없는 길이, canonical escape, valid UTF-8와 776-byte encoded cap을 검증한다. | packed package와 다른 언어가 기록한 authority key의 Store 상호운용 확인 |
| `NODE-ROUTE-006` | `88ccb7005e` | `npm run build`, `service-selection.test.js` 6/6 PASS. Cycle plan은 `select()`마다 cumulative state Map을 복사하지 않고 candidate rebuild 직전에 현재 cursor state를 materialize한다. 분포와 membership 변경 후 credit 보존은 유지한다. | packed package benchmark에서 candidate 수 증가에 따른 select latency와 allocation 확인 |
| `NODE-DISP-001`, `NODE-DISP-002` | `7429193a90` | `npm run build`, `backend-contract.test.js` 42/42, `npm run verify:m6a-runtime` 36/36, `npm run verify:m6b-runtime` 87/87 PASS. 원격 generic·stateful Request는 mailbox 포화 시 canonical `CapacityExceeded` reply를 받고, Application drain은 owner 하나씩 claim하여 느린 handler와 무관한 owner를 별도 drain에서 진행한다. | packed package와 다중 owner process 부하에서 즉시 terminal, owner별 진행과 bounded host HWM 확인 |
| `NODE-DISP-003` | `490a1f651b` | `npm run build`, `npm run verify:m6b-runtime` 88/88 PASS. Local multicast, Actor lifecycle control과 Actor binding control이 mailbox 포화로 거부되면 Stateful runtime이 record kind와 owner를 보고하고 runtime manager가 `Backpressure/Drop` dispatch diagnostic을 게시한다. Actor binding control 포화 회귀는 성공 reply와 별도로 drop 관찰 기록이 남는지 검증한다. | packed package와 실제 lifecycle·multicast 부하에서 diagnostic cardinality, 남은 Actor/Spot 상태의 후속 reconciliation 확인 |
| `NODE-DISP-005` | `41a72123dd` | `npm run build`, `backend-contract.test.js` 45/45와 `backend-contract.test.js` + `entry-spot-serial-dispatch.test.js` 결합 실행 71/71 PASS. Mesh pump와 process 단위 event-loop queue가 현재 실행 영역을 `application` 또는 `infrastructure`로 표시한다. Completion, send-ready와 transfer control은 infrastructure 영역에서만 실행하며 application 영역에서 호출하면 queue에 넣지 않고 typed `InvalidOperation`으로 종료한다. | packed package와 실제 handler 대기 process에서 completion·timeout·shutdown·peer admission 진행, 잘못된 infrastructure 진입의 즉시 terminal 확인 |
| `NODE-DISP-006` | `9a04c1c1d9` | `npm run build`, `backend-contract.test.js` + `channel-client.test.js` 145/145 PASS. RouteMesh backend의 1 ms poll과 process 공용 channel receive idle waiter의 5 ms poll을 이름 있는 내부 정책으로 고정했다. Node 한국어·영문 internals는 두 값이 event loop가 즉시 timer를 실행할 때의 최선 지연 하한이며 부하로 더 길어질 수 있음을 공표한다. | packed package와 실제 socket의 idle-to-readable 지연 분포, application handler 대기 중 completion·liveness 진행 확인 |
| `NODE-DISP-007` | `25515d85c7` | `npm run build`, `npm run verify:m6a-runtime` 36/36, `backend-contract.test.js` 46/46 PASS. Raw Mesh receive는 peer별 64건·4 MiB와 wake 전체 2 ms 중 먼저 닿는 한도에서 멈추고 Core ROUTER의 fair-queue cursor 다음 pipe에서 이어진다. Mailbox dispatch도 16 receive batch 또는 2 ms 중 먼저 닿으면 timer phase에 양보한다. Raw runtime 회귀는 source peer와 실제 multipart byte 계상이 poll owner까지 전달되는지 검증한다. | packed package와 2개 이상 peer 포화 process에서 후속 peer 지연, count·byte·elapsed 각 경계와 send-ready 진행 확인 |
| `NODE-DISP-008` | `e37522d21e` | `npm run build`, `npm run typecheck`, `npm run verify:m6a-runtime` 37/37, channel·fanout·route 관련 결합 contract test 159/159 PASS. Subscriber와 route receive loop는 bounded in-flight tracker에 dispatch를 넘긴 뒤 다음 수신을 진행하며, 첫 handler가 대기 중이어도 두 번째 subscriber record가 dispatch되는 회귀를 포함한다. Generic Mesh request의 malformed frame은 correlation을 복구할 수 있으면 `ProtocolError`와 framework error code 16으로 reply하고, malformed one-way는 `InvalidFrame/Drop` 진단을 남긴다. Reply route가 아직 없는 empty admission probe 실패는 malformed frame이 아니므로 `dropped`로 분리한다. | packed package와 느린 subscriber·route handler process에서 수신 진행, 1,024 in-flight 상한, malformed request terminal과 one-way 진단 확인 |
| `NODE-EXEC-001` | `992955d842` | `npm run build`, `entry-spot-serial-dispatch.test.js` 26/26, `npm run verify:m6b-runtime` 87/87 PASS. 자기 Spot으로 보내는 one-way는 bounded queue가 인수한 뒤 송신자 turn을 반환하고 기존 대기 작업 뒤에서 실행한다. 같은 turn에서 결과를 기다리는 request는 handler 실행 전에 `InvalidOperation`으로 종료한다. | packed package와 실제 Spot handler process에서 FIFO, capacity terminal과 one-way handler 오류 진단 확인 |
| `NODE-EXEC-002` | `382e01410f` | `npm run build`, `npm run typecheck`, `entry-spot-serial-dispatch.test.js` 27/27, `npm run verify:m6b-runtime` 87/87 PASS. 같은 runtime의 one-way Spot send는 queue가 찼을 때 bounded FIFO waiter에서 send timeout까지 기다리고, waiter 한도를 넘으면 `DeadlineExceeded`로 종료한다. Submit은 handler 결과가 아니라 queue admission에서 완료하며 handler 오류는 diagnostics로 분리한다. | packed package와 실제 local Spot 부하에서 timeout·cancellation, FIFO admission과 bounded waiter HWM 확인 |
| `NODE-EXEC-003`, `NODE-EXEC-004` | `c6d3ff8535` | `npm run build`, `npm run typecheck`, `npm run verify:m6b-runtime` 89/89, `npm run verify:m6c-runtime` 80/80 PASS. Yield continuation과 application callback을 실행하는 barrier turn은 lifecycle capacity를 사용하지 않고 기존 application FIFO 뒤에서 실행한다. Yield 시 shared Spot execution claim을 즉시 반환하고 continuation이 도착하면 seal 여부를 동기적으로 확인해 새 claim을 얻는다. Unit seal이 먼저 확정됐으면 대기하지 않고 `SpotMoving`을 public `Unavailable`로 반환한다. Actor queue claim은 원래 handler의 owner Promise가 계속 보유하므로 같은 Actor의 다음 job보다 continuation이 먼저 끝나는 규칙은 유지한다. | packed package와 실제 장시간 원격 request·동시 relocation process에서 seal quiescence, continuation의 단일 `Unavailable` terminal과 application/lifecycle HWM 분리 확인 |
| `NODE-LAYER-003`, `NODE-WIRE-007` | `18b02c9b0c`, `ec524deca7` | `npm run build`, `npm run typecheck`, `npm run verify:m6b-runtime` 90/90, `npm run verify:m6c-runtime` 80/80, actor·backend 결합 contract test 126/126 PASS. Mesh completion과 Actor Join이 공유하는 128-bit operation identity의 map key 생성과 non-zero CSPRNG 생성을 하나의 내부 value object로 모았다. Zero entropy는 새 값을 생성할 때까지 다시 뽑고 frozen wire request의 zero operation identity는 decode에서 거부한다. `bigint`는 증가 중 overflow하지 않으므로 도달할 수 없던 ReplyRouteId overflow 분기를 제거했다. Session `requestSeq`와 envelope `correlationId`는 서로 다른 protocol domain의 식별자이므로 128-bit operation identity로 변환하지 않는다. | packed package에서 operation identity key·zero rejection과 Actor Join completion correlation을 확인하고, 장시간 handoff process에서 ReplyRouteId 중복이 없는지 확인 |
| `NODE-OWN-006` | `289b6eef3a` | `npm run build`, `npm run typecheck`, codec·Actor·Spot 결합 contract test 172/172 PASS. 불변 serializer registry마다 outbound business runtime type을 key로 하는 선택 cache를 만들고, 같은 constructor의 후속 payload는 `canSerialize` 후보를 다시 순회하지 않는다. JSON fallback도 cache하며 constructor는 `WeakMap` key이므로 동적으로 만들어진 type을 runtime 수명까지 붙잡지 않는다. | packed package benchmark에서 serializer 수와 무관한 warm-path selection 횟수·allocation, 서로 다른 business type의 codec 선택 확인 |
| `NODE-OWN-003`, `NODE-OWN-004`, `NODE-OWN-005` | `aac5f84341`, `b0e74a0e3a` | `npm run build`, `npm run typecheck`, `npm run verify:m6a-runtime` 37/37, backend·STREAM·Actor·Spot 결합 contract test 357/357 PASS. Public `ZLinkEncodedPayload` 입력과 `data()`·`toBytes()`는 defensive copy를 유지한다. Runtime이 소유한 `Buffer`는 internal storage가 그대로 인수하고 JSON lazy decode는 빈 길이 확인과 문자열 변환을 위해 byte array를 복사하지 않는다. Completion과 routed join snapshot은 한 번만 복사하며 managed stream은 native `Message.from` 앞의 중복 복사를 제거했다. Raw mailbox는 accepted record를 native part로 옮긴 직후 원본 envelope 참조를 해제한다. Lazy decoder는 원본 `Message` 대신 인수한 payload만 보관한다. `Buffer`가 아닌 `Uint8Array` adapter 입력은 복사해서 Node `Buffer` 문자열 의미를 보장한다. | packed package와 clean consumer에서 public defensive copy·extension codec decode를 확인하고, 대형 multipart process benchmark에서 claim·handler 구간의 peak retained bytes와 managed stream·completion copy 횟수 확인 |
| `NODE-OBS-002` | `c383f447bc`, `ebfb874dbf` | `npm run build`, `npm run typecheck`, message-flow·STREAM·Actor·Spot 결합 contract test 278/278 PASS. Message-flow가 꺼져 있고 ambient flow도 없으면 `AsyncLocalStorage.run(undefined, ...)`을 호출하지 않고 application callback을 직접 실행한다. Parent inbound flow를 명시적으로 지우는 nested 경로는 기존 `AsyncLocalStorage` 경계를 유지한다. 첫 transition이 사용한 live mode는 tracer별 `WeakMap`에 flow 수명으로 고정하므로 같은 message의 후속 transition은 mode 변경 전후로 일부만 기록되지 않는다. Ambient flow 밖의 조회는 live cell을 계속 즉시 반영한다. | packed package benchmark에서 disabled path allocation·latency와 enabled flow snapshot의 GC 해제 확인 |
| `NODE-WIRE-009` | `8b8a643471` | `npm run build`, `npm run typecheck`, ClientServer·channel 결합 contract test 124/124 PASS. Client와 Server 양쪽 liveness 수신은 현재 physical connection·routing identity·outstanding probe ID와 맞지 않는 ACK를 readiness 갱신에 사용하지 않는다. 이전처럼 무음 폐기하지 않고 `RequestProtocolError` 내부 진단을 runtime failure sink에 한 번 게시한다. | packed package와 reconnect process에서 stale·duplicate ACK 진단 cardinality와 current ACK readiness 갱신 확인 |
| `NODE-RELOC-003`, `NODE-SESS-003`, `NODE-LIFE-004` 설계 판정 | 해당 없음 | Common spec과 실제 owner 경계를 다시 검토했다. Host relocation phase는 workload aggregate의 capture·publish·replay·cleanup을, remote Actor Join phase는 한 Actor membership transaction의 admission·commit·abort를 표현하므로 서로 다른 bounded context다. `ServiceMailbox`, `EventLoopWorkQueues`, `RouterOperationQueue`도 각각 relocation 가능한 owner mailbox, process infrastructure/application 진행, native request slot 순서를 소유하므로 공통 base로 합치면 서로 필요 없는 상태와 규칙이 누출된다. Node internals는 실제 serial scheduler 기본값인 application 4,096건/16 MiB, lifecycle 1,024건/4 MiB와 byte·시간·burst 회계를 한영 문서에 기록했다. | 문서 link·style gate와 packed package에서 기본 scheduler 한도 확인 |
| `NODE-LAYER-001` | `1f8efb75f5` | `npm run build`, `topology-runtime-projection.test.js` 21/21, `npm run verify:m6c-runtime` 80/80 PASS. Shutdown은 즉시 host admission을 닫고 `Draining`을 게시한다. relocation scheduler는 이미 permit을 받은 unit의 terminal을 기다리되 대기 unit을 새로 시작하지 않으며 Relocate waiter를 `Blocked/ShutdownRequested`로 끝낸다. | packed package의 concurrent Relocate·Shutdown process에서 terminal 재호출, active unit commit과 shutdown deadline 확인 |
| `NODE-RELOC-002` | `d084e0250b` | `npm run build`, `npm run verify:m6c-runtime` 80/80 PASS. SpotWide aggregate는 wire Message Follow ingress seal 직후 같은 event-loop turn에서 execution barrier를 seal한다. Actor session 준비는 두 seal 뒤에 완료하므로 direct message callback이 두 경계 사이에 진입할 수 없다. | packed package의 장시간 turn·동시 request·relocation process에서 source 정상 처리 또는 Message Follow 인계와 exactly-once terminal 확인 |
| `NODE-COMP-001`, `NODE-COMP-002` | `b0eccff662` | `npm run build`, `backend-contract.test.js` 43/43, `actor-manager.test.js` 79/79, `npm run verify:m6b-runtime` 87/87, `npm run verify:m6c-runtime` 80/80 PASS. 모든 Mesh request 경로는 native submit과 waiter 등록을 같은 event-loop turn에 수행한다. 이른 완료를 보관하던 `arrived` 맵을 제거했으므로 timeout 뒤 늦은 payload는 저장하지 않는다. STREAM 관련 test는 148/150이며 기존 disconnect callback 완료 대기 두 항목만 단독 실행에서도 실패한다. | packed package의 대량 timeout·late reply process에서 completion table HWM이 요청량과 함께 증가하지 않는지 확인하고, 기존 STREAM blocker를 별도 수정한 뒤 전체 suite 재실행 |
| `NODE-COMP-004` | `116e27ad68` | `npm run build`, `backend-contract.test.js` 44/44, `npm run verify:m6b-runtime` 87/87, `npm run verify:m6c-runtime` 80/80 PASS. Actor join과 STREAM bind 재시도는 `result=NotConnected` 또는 Framework `RouteNotConnected` kind로만 분류한다. Error message가 같은 일반 오류는 재시도하지 않고, 진단 문구가 달라도 typed result가 같으면 재시도한다. STREAM 관련 test는 기존 disconnect callback 두 항목을 제외한 148/150이 통과했다. | packed package와 실제 peer reconnect process에서 OS별 진단 문구와 무관한 재시도, deadline과 중복 submit 부재 확인 |
| `NODE-COMP-003` | `3ba9ad6e29` | `npm run build`, `npm run verify:m6b-runtime` 87/87 PASS. Source Spot callback의 typed request와 raw request는 공통 `ZLinkDeferredCompletion`이 terminal claim, abort와 late value 소유권을 한 번만 확정한다. `channel-client.test.js`의 abort 뒤 late raw reply close를 포함한 focused 2/2와 재실행 100/100이 통과했다. 첫 전체 실행은 별도 bootstrap route가 `NotFound`여서 99/100이었고 같은 두 항목 focused 및 전체 재실행에서는 재현되지 않았다. | packed package와 실제 callback transport에서 abort·reply 경합을 반복해 exactly-once terminal과 late `Message` 해제를 확인 |
| `NODE-SESS-002` | `4eb9dad69c`, `570617f5d1` | `npm run build`, `stream-session-runtime.test.js` 53/53과 관련 STREAM test 150/150 PASS. 유효한 PING/PONG은 application claim과 session application FIFO를 거치지 않고 transport callback의 runtime 경로에서 처리한다. Disconnect와 dispose는 먼저 큐에 들어온 lifecycle turn이 끝난 뒤 transport를 닫고, callback 실패 여부와 무관하게 binding cleanup을 실행한다. | packed package와 실제 peer의 handler 지연·heartbeat·disconnect process에서 false timeout 부재, callback 순서와 Actor binding cleanup 확인 |
| `NODE-SESS-004` | `7508ec2dcf` | `npm run build`, `npm run typecheck`, Actor·STREAM contract test 177/177 PASS. Replacement bind를 먼저 제출하므로 bind가 실패하면 기존 native·logical binding을 그대로 유지한다. Bind가 성공한 뒤 remote confirmation이 실패하면 새 binding을 해제하고 이전 binding과 confirmation을 복원한다. 성공한 replacement의 terminal 뒤에만 logical route owner를 바꾼다. | packed package와 실제 reconnect process에서 replacement 진행 중 기존 route 사용, 성공 뒤 원자 전환과 stale disconnect fencing 확인 |

---

## 2. 우선 대응 항목 (심각도 상)

| ID | 요약 | 문서 |
|---|---|---|
| NODE-DISP-001 | 대상 mailbox 포화 시 원격 Request가 **응답 없이 조용히 폐기** — 호출자는 타임아웃까지 대기, 진단 불가 | 03 §6 |
| NODE-DISP-002 | 디스패치 펌프가 32개 owner를 claim 후 각 핸들러 완료를 순차 await — 느린 핸들러 1개가 최대 31개 무관 owner를 블로킹 | 07 §3/§4 |
| NODE-RELOC-002 | wire seal→execution seal 사이 창에서 admission waiter가 미분류 Error로 거부 — **one-way 메시지 손실**, Request는 일반 내부 오류로 노출 | 05 §1/§2 |
| NODE-EXEC-001 | 스펙이 금지한 재진입 허용 — 자기 Spot으로의 로컬 전송이 큐 뒤가 아니라 **송신자 turn 안에서 인라인 실행** | 02 Pitfall 5 |
| NODE-COMP-001 | 완료 테이블 `arrived` 맵이 무한 — 타임아웃 후 도착한 응답(payload Buffer 포함)이 노드 dispose까지 영구 누적 | 04 §3, 03 §6 |
| NODE-SESS-001 | 크로스 노드 세션 스왑 시 이전 소유자 tombstone·정리 확인 절차 부재 — 이전 노드의 stale 바인딩이 다음 실패까지 잔존 | 09 §3 |
| NODE-SESS-002 | STREAM 하트비트 PING/PONG이 애플리케이션 레인에 실림 — 앱 레인이 5초 이상 밀리면 통신 가능한 session을 `heartbeat_timeout`으로 오판 | 09 §1 |
| NODE-LAYER-001 | 진행 중 relocation과 shutdown 경합 시 스펙(shutdown 승리)과 반대로 **relocation 전체 완료를 대기** | 01 §3 |
| NODE-WIRE-001 | RouteMesh에 금지된 message-size 설정·admission 필드가 남아 있고, ClientServer는 push된 update로 **협상된 한도를 연결 중 변경 가능** | 12 §2/§4 |
| NODE-OWN-001 | 수신 content-type 미사용 + 미등록 시 invalid JSON이 **raw 텍스트로 조용히 fallback** — `ProtocolError` 미발생 | 11 §7 |
| NODE-CONTRACT-DIAG-002 | 제거 대상 public observer·sink·raw DTO와 file·label이 package에 남아 있고 표준 logger provider 대체 증거가 없음 | DEC-01 |
| NODE-CONTRACT-STREAM-001 | STREAM send별 admission timeout이 없어 같은 session에서 call마다 다른 deadline을 표현할 수 없음 | DEC-09 |
| NODE-CONTRACT-RELOC-001 | 64 MiB 초과 participant state가 `StateIncompatible`이 아닌 `RelocationFailed`로 끝남 | DEC-17 |

### NODE-DISP-001 — 원격 Request 무응답 폐기
`enqueueFrame`이 mailbox `tryEnqueue` 실패 시 `'protocolError'`를 반환할 뿐 보관된 reply 포트를 호출하지 않고, backend `poll()`은 그 결과를 완전히 무시한다. 스펙 03 §6이 반례로 기술한 "대기 중인 호출자가 아무 통지도 받지 못하고 타임아웃까지 매달리는" 패턴 그대로다(스펙 12 「5.3」: `CapacityExceeded`/`Unavailable`로 종료 요구). 로컬 fast path는 terminal 109로 올바르게 완료된다 — 원격 경로만 결손.
- 증거: `node/runtime/foundation/service-stateful-runtime.ts:3008-3028`, `node/runtime/foundation/raw-service-mesh-runtime.ts:796-813`, `node/runtime/backend/node/node-raw-mesh-backend.ts:1330-1334`

### NODE-DISP-002 — cross-owner head-of-line 블로킹
mesh 디스패치 펌프는 최대 32개 ready owner를 한 배치로 claim한 뒤 **각 레코드의 핸들러 완료를 순차 `await`**한다. claim은 핸들러 종료 후 `release()`까지 mailbox 배타를 유지하므로, owner A의 핸들러가 느린 원격 응답을 기다리는 동안 같은 배치에 claim된 나머지 owner들은 처리도, 다른 drain에 의한 재claim도 불가능하다. 이 레벨에는 시간 예산이 없다(50 ms 예산은 각 owner 자체 스케줄러 내부에만 있음). "실행 자원은 집합에서 owner **하나**를 가져온다"와 "한 owner의 예산 소진 시 다른 owner 진행" 확인 항목 위반.
- 증거: `node/runtime/backend/mesh-dispatch-pump.ts:147-216`, `node/runtime/foundation/service-mailbox.ts:144`

### NODE-RELOC-002 — seal 사이 창의 메시지 손실
소스 측 블로킹 지점이 두 개(wire 레벨 `sealSpotMessageFollowIngress` → execution barrier seal)인데, wire seal은 통과했지만 실행을 시작하지 못한 메시지가 그 사이에 낀다. `waitForQuiescence`는 **active claim만** 기다리고 `admissionWaiters`에 대기 중인 turn은 배수하지 않으며, capture는 `queuedMessages: []`로 실어 보내고, `commit()`은 모든 admission waiter를 미분류 `Error('ZLink execution barrier is committed.')`로 거부한다 — 어느 dispatcher도 이를 follow 큐로 재라우팅하지 않는다. 결과: 이 창의 수락된 메시지는 스팬 ①(정상 처리)도 스팬 ②(보류 후 인계)도 아니고, one-way는 손실되며 Request는 일반 내부 오류로 호출자에게 노출된다.
- 증거: `node/runtime/execution/index.ts:221-259`, `node/runtime/spots/spot-serial-executor.ts:134-146`, `node/runtime/host/service-relocation-host-runtime.ts:685-712`

### NODE-EXEC-001 — 재진입 허용 (스펙: 금지)
`ZLinkSpotSerialExecutor.execute`의 docstring이 명시하듯 현재 turn 내부에서의 호출은 큐잉 없이 그 turn의 일부로 인라인 실행된다. 공개 Spot request 빌더 두 곳과 `executeOnSpot`에는 가드가 있으나, **라우팅된 로컬 디스패치 경로는 무가드**: 핸들러가 자기 Spot에 보낸 one-way가 `sendToSpotHandle → … → activation.serial.execute` 체인으로 송신자 turn 안에서 실행된다. 결과: (1) 먼저 큐잉된 작업보다 앞서 실행되어 FIFO 위반, (2) 인라인 경로는 `reserve`를 호출하지 않아 레인 한도 완전 우회, (3) 대상 핸들러 예외가 송신자에게 전파. 스펙은 no-wait 자기-전송도 큐 뒤에 붙일 것을, 대기형은 제출 전 `InvalidOperation` 실패를 요구한다. `spot-outbound.ts:544-546`의 "valid FIFO admission" 주석은 실제 동작과 모순.
- 증거: `node/runtime/spots/spot-serial-executor.ts:80-93`, `node/runtime/spots/spot-routed-spot-packet-dispatch.ts:120`, `node/runtime/spots/spot-outbound.ts:544-547`

### NODE-COMP-001 — 완료 테이블 무한 누적
`ZLinkMeshCompletionTable`의 `arrived` 맵은 한도·오버플로 경로·축출이 없다. waiter가 abort/타임아웃하면 pending 엔트리가 삭제되므로, 늦게 도착한 완료는 항상 `arrived`에 저장되어 message Buffer와 함께 테이블 dispose까지 잔존한다. 스펙 §3의 "보관 슬롯은 유한하고 초과는 관찰 가능한 `CapacityExceeded`로 끝난다" 위반이며, 타임아웃 볼륨에 비례하는 메모리 누수다. C++의 `_completed_operations` 무한 누적(CPP-COMP-004)과 동일 계열.
- 증거: `node/runtime/backend/mesh-completion-table.ts:24, 40-49, 69-72`

### NODE-SESS-001 / NODE-SESS-002 — 세션 바인딩
**NODE-SESS-001**: 다른 노드의 세션에 이미 bind된 Actor에 `boundSessionBind`가 오면 `installSessionBinding`으로 즉시 교체하고 `Ok`를 응답 — 이전 세션 소유 노드에 tombstone을 보내지 않고 정리 확인도 기다리지 않는다(스펙 20 §4 절차 미구현). 이중 전달은 `bindingGeneration` fence의 사후 거부로만 방어되고, 이전 세션은 다음 전송이 실패할 때에야 스왑을 인지한다. C++ CPP-SESS-001과 동일 계열 — **두 런타임 모두 결손이므로 크로스 언어 공통 항목으로 기록 필요**.
- 증거: `node/runtime/foundation/service-stateful-runtime.ts:2936-2971`, `node/runtime/foundation/service-stateful-registry.ts:340-354`, `node/runtime/host/actor-packet-relay.ts:245-284`

**NODE-SESS-002**: STREAM session의 모든 inbound frame — Control 하트비트 PING/PONG 포함 — 이 `enqueueApplication`으로 애플리케이션 레인에 큐잉되고 admission claim까지 잡는다. PONG 처리(`awaitingPongSince` 해제)가 밀린 비즈니스 dispatch 뒤에서 FIFO로 처리되므로, 앱 레인이 5초(`ZLINK_STREAM_HEARTBEAT_TIMEOUT_MS`) 이상 밀리면 lifecycle 레인의 liveness 검사가 통신 가능한 session을 `heartbeat_timeout`으로 오판한다. Mesh ingress는 inline으로 처리하고 ClientServer는 timer가 직접 drain하므로 이 문제는 STREAM session에만 있다.
- 증거: `node/runtime/streams/stream-session-runtime.ts:274-326, 401-505, 507-555, 658-687`

### NODE-LAYER-001 — shutdown이 진행 중 relocation에 양보
스펙(01 §3, 28 §11): 겹치면 shutdown 승리 — 현재 이동 유닛만 확정하고 나머지는 시작하지 않으며 relocation waiter는 `Blocked/ShutdownRequested`로 종료. Node `runShutdown`은 `runtimeState === Relocating`이면 `await this.relocationOperation`으로 **멀티 유닛 이동 전체가 끝나기를 기다린 뒤** admission seal을 적용하고, relocation 호출자는 relocation 자체 결과를 받는다. 긴급 shutdown이 relocation deadline까지 지연될 수 있다. (shutdown 시작 후의 **신규** relocate 거부는 올바르게 구현됨 — in-flight 경우만 결손.)
- 증거: `node/runtime/host/index.ts:887-899` (올바른 신규 거부는 `:751-758`)

### NODE-WIRE-001 — RouteMesh 금지 상한 잔존·ClientServer 협상값 변경 가능
현재 common contract에서 RouteMesh ServerServer에는 Framework-level message-size 설정·협상·거부가 없다
(`common/spec/07-channel-topology.ko.md:609-634`,
`common/internals/12-service-wire-protocol.ko.md:91-110`). 따라서 RouteMesh 송수신 경로에
`min(local, remote)` clamp가 없다는 사실은 gap이 아니다. 실제 gap은 public
구현의 `ZLinkMeshNodeSocketConfig.maxMessageSize`와 mutable router socket 설정을 노출하고, RouteMesh
descriptor에 설정과 무관한 `effectiveMaxMessageBytes = 4 MiB`를 넣어 codec·validation까지
유지한다는 점이다. Node exact interface는 common contract에 맞게 해당 field와 negotiated-bound
설명을 제거했다. Application HWM validation도 RouteMesh의
`maxMessageSize`를 bounded-listener 조건으로 사용하므로 이 의존성을 함께 제거해야 한다.
이 RouteMesh 전용 설정과 admission/wire field를 제거해야 한다.

ClientServer에는 별도의 negotiated bound 계약이 남는다. 그런데
`applyClientServerDescriptorUpdate`가 `normalizedEffectiveMaxMessageBytes`를 고정 필드에서 누락해,
서버가 push한 `update`로 협상된 한도를 재admission 없이 변경할 수 있다(스펙 §4: 재admission으로만
변경 가능). RouteMesh의 금지된 bound 잔존과 ClientServer의 협상값 불변성 위반은 같은 ID에서
경로를 구분해 추적한다.
- 증거: `node/contracts/Configuration/Builders.ts:102-110`,
  `node/contracts/Configuration/RegistrationBuilders.ts:902-905,1615`,
  `node/contracts/Configuration/RegistrationValidators.ts:58-90`,
  `node/contracts/Configuration/Configs.ts:3-12`,
  `node/runtime/channels/channel-socket-options.ts:22-72,113-129`,
  Node exact interface `interfaces/01-foundation-configuration.ko.md:167-175`,
  `interfaces/02-channel-messaging.ko.md:285-290`,
  `node/runtime/backend/node/node-raw-mesh-backend.ts:1295`,
  `node/runtime/foundation/service-wire-m6a-codec.ts:257-304`,
  `node/runtime/foundation/service-topology-registry.ts:28,442-447,547`,
  `node/runtime/channels/channel-socket-registry.ts:1304-1311,1648,1698-1710`

StreamNode의 크기 상한 적용 자체는 이 gap에 해당하지 않는다. `maxMessageSize` 기본값은 64 KiB이고 registration 값이
Core STREAM socket과 frame reassembler에 전달되며, 6-byte prefix를 제외한 header+payload를
inbound client→server complete message에서 검사한다. outbound server→client에는 이 제한을
적용하지 않는다. 다만 초과 시 `EMSGSIZE`와 진단 trace를 남기는 계약은
`NODE-STREAM-SIZE-001`로 별도 추적한다. 증거: `node/contracts/Configuration/InternalDefaults.ts:4`,
`node/runtime/streams/index.ts:197-202`, `node/runtime/streams/stream-session-runtime.ts:748-759,1003`,
`node/runtime/streams/stream-frame-reassembler.ts:72-83,128-137`.

### NODE-OWN-001 — 수신 content-type 미사용 + 무음 텍스트 fallback
mesh/actor/spot/stream 수신 계열은 와이어 content-type(및 stream 헤더 `codec` enum)을 역직렬화기 선택에 전혀 넘기지 않는다. `decodeFrameworkPayload`는 기본 serializer만 선택하고, serializer 미등록 시 invalid JSON이 **raw UTF-8 텍스트로 조용히 반환**된다(`rejectInvalidJson=false` 기본값; `ZLinkMessage.decode()`도 동일). 타 런타임의 비-JSON 페이로드가 `ProtocolError` 대신 오파싱되거나 깨진 텍스트로 전달된다. 채널 envelope 경로는 content-type 조회 + 명시적 `ProtocolError`로 올바름 — 그 외 경로가 결손. C++ CPP-OWN-001과 동일 계열(스펙 11 §7이 익명으로 기술한 "위반 구현"군).
- 증거: `node/runtime/messaging/payload-codec.ts:82-123`, `node/runtime/spots/spot-actor-packet-dispatch.ts:227-229`, `node/contracts/Common/ZLinkMessage.ts:33-38`

---

## 3. 문서별 상세

### 3.1 01-layering

| ID | 분류 | 요약 |
|---|---|---|
| NODE-LAYER-001 | GAP·상 | §2 참조 — shutdown이 in-flight relocation에 양보 |
| NODE-LAYER-002 | PARTIAL·중 | `RoutingId`/`ActorId`/`SpotId`가 모두 string alias인 것은 현재 Node exact interface 자체가 고정한 public declaration이므로, public alias 존재만으로 구현 GAP이라고 할 수 없다. 다만 runtime 내부에서도 별도 nominal 타입 없이 값을 섞고, `RoutingId`를 평문 string과 `ZLinkOpaqueRoutingId` 사이에서 storage hex로 재인코딩한다. 따라서 exact interface 변경이 아닌 내부 표현·비용 개선 항목으로 범위를 축소한다. 증거: Node exact interface `interfaces/01-foundation-configuration.ko.md:13-14, 85`, `node/contracts/Common/CoreTypes.ts:2-6`, `node/runtime/routing-id.ts:57, 80-119` |
| NODE-LAYER-003 | PARTIAL·하 | 호출 진행 식별자가 `MeshOperationId {high,low}` / `"high:low"` 문자열 키 / 세션별 `bigint requestSeq` / `correlationId` 문자열로 다중 포맷 + `OperationId` 이름이 3개 이상의 개념에 사용. 증거: `node/runtime/backend/mesh-completion-table.ts:96-97`, `node/runtime/spots/spot-actor-packet-dispatch.ts:203` |

만족(요약): 바인딩 타입 격리(`runtime/backend/node/`에만 import), 프로토콜 중복 없음(stream-wire 공유), shutdown 로직 런타임 소유·순서 고정, 동종 중복 병합, blocked 결과 비저장(§3 — C++ CPP-RELOC-001과 달리 `resetBlockedRelocation`으로 올바름), 등록 1회 검증.

### 3.2 02-serialization

| ID | 분류 | 요약 |
|---|---|---|
| NODE-EXEC-001 | GAP·상 | §2 참조 — 재진입 인라인 실행 |
| NODE-EXEC-002 | GAP·중 | Pitfall 2 결과 표의 send/one-way 행 위반: 로컬 send 경로에 대기 슬롯이 없어 레인 포화 시 즉시 `CapacityExceeded`(Request 행의 결과)로 거부. 또한 routed 로컬 one-way의 `submit()`이 대상 핸들러 실행 완료까지 resolve되지 않아 완료 시점이 admission이 아닌 핸들러 완료에 결합되고 핸들러 오류가 송신자에 도달. (transport/MeshNode send의 SEND_READY 대기 기제는 올바름 — 로컬 경로만 결손.) 증거: `node/runtime/execution/serial-scheduler.ts:113-116`, `node/runtime/spots/spot-routed-spot-packet-dispatch.ts:114-143` |
| NODE-EXEC-003 | PARTIAL·중 | lifecycle 레인 오염: yield 후 재개된 **애플리케이션** turn과 barrier turn이 lifecycle 레인으로 라우팅되어 lifecycle 한도를 소비하고 앞서 큐잉된 앱 작업보다 우선 실행 — 두 레인의 공정성 계산이 스펙이 정의하지 않은 혼합 모집단 위에서 수행됨. 증거: `node/runtime/spots/spot-serial-executor.ts:139-146, 215-221` |
| NODE-EXEC-004 | PARTIAL·중 | §6 seal 우선순위 역전: yield된 핸들러의 barrier claim이 최종 완료까지 유지되어 `waitForQuiescence`가 재개 실패 대신 **중단된 핸들러를 기다림** — 느린 원격 응답에 yield된 핸들러가 relocation capture를 deadline abort까지 지연. 증거: `node/runtime/spots/spot-serial-executor.ts:167-207`, `node/runtime/execution/index.ts:197-241` |

만족(요약): per-actor 큐→공유 gate, PerActor 모드, 2-lane 별도 한도, lifecycle 우선+burst 8+debt, 앞끼워넣기 없음(`prepend`는 FIFO 보존 복원), 실행 자원 비비례, yield 제한(SpotWide User/Instance만, 제출 전 실패).

### 3.3 03-progress-isolation

| ID | 분류 | 요약 |
|---|---|---|
| NODE-DISP-001 | GAP·상 | §2 참조 — 원격 Request 무응답 폐기 |
| NODE-DISP-003 | PARTIAL·중 | `actorJoined`/`boundSessionBind` 등 lifecycle 컨트롤 레코드와 로컬 multicast 대상의 `tryEnqueue` 실패가 관찰 기록조차 없이 무시됨 — 컨트롤 레코드 유실은 actor 멤버십/바인딩 상태를 진단 흔적 없이 고착시킬 수 있음. 증거: `node/runtime/foundation/service-stateful-runtime.ts:3064-3103+` |
| NODE-DISP-004 | PARTIAL·중 | 채널 ROUTER/subscriber/route 수신 루프는 HWM 도달 시 소켓의 **모든** 수신을 정지 — 같은 소켓에 다중화된 인프라 프레임도 함께 정지(분류 스팬 R 없음). MeshNode 경로만 스펙 형태(앱 도메인만 gate) 구현. 증거: `node/runtime/channels/channel-receive-loops.ts:203-205, 361-363, 491-493` |
| NODE-DISP-005 | PARTIAL·하 | 실행 영역 마커·오류 경로 부재 — "앱 컨텍스트에서 인프라 전용 작업 호출 시 대기 없이 실패" 확인 항목을 관찰할 수 없음(잘못된 조합이 무음 통과). 증거: `node/runtime/execution/serial-scheduler.ts:1`, `node/runtime/execution/index.ts:21, 123-134` |

(완료 테이블 무한 누적은 NODE-COMP-001로 §3.4에 기재.) 만족(요약): 2도메인 mailbox+인프라 claim 예산, 관찰 비차단(coalescing+손실 카운터), send 계열 bounded wait+단일 재제출+`DeadlineExceeded`, `Backpressured` 비노출, R 고정, HWM 자동 산정 순서.

### 3.4 04-completion

| ID | 분류 | 요약 |
|---|---|---|
| NODE-COMP-001 | GAP·상 | §2 참조 — `arrived` 맵 무한 누적 (03 §6의 "무한 적치"이기도 함) |
| NODE-COMP-002 | GAP·중 | §3 결정 미구현: 모든 backend 요청이 operation id를 submit의 **출력**으로 반환하고 완료 대기 등록이 그 뒤 — join 경로는 submit과 `wait` 사이에 실제 `await`가 있어 이른 응답이 실재하고 보관 슬롯이 하중을 짊어짐. 스펙이 제거하려 한 구조가 영구화. 증거: `node/runtime/actors/actor-client.ts:446-450`, `node/runtime/actors/actor-local-native-join.ts:702-707` |
| NODE-COMP-003 | GAP·중 | 완료 확정 방식 3종 이상 공존(pending 맵 삭제+arrived 슬롯 / `completed` 플래그 / ad-hoc `settled` 클로저) — C++ CPP-COMP-002와 동일 계열. 증거: `node/runtime/backend/mesh-completion-table.ts:60-77`, `node/runtime/messaging/index.ts:570-604`, `node/runtime/channels/source-spot-router.ts:28-74` |
| NODE-COMP-004 | PARTIAL·중 | §6 문자열 분류: join/bind 재시도 분기가 `error.message.includes('Transport endpoint is not connected')` 등 부분 문자열 매칭 — 문구 변경 시 재시도 동작이 조용히 바뀜. C++ CPP-COMP-001과 동일 계열(단 Node는 완료 경로 자체는 타입 분류로 올바르고 disconnect 분기만 결손). 증거: `node/runtime/actors/actor-local-native-join.ts:860-865, 899-905`, `node/runtime/streams/managed-stream.ts:411-416` |

만족(요약): 단일 claim 구조의 exactly-once, 늦은 응답 재확정 불가, 수락 후 자동 재전송 금지(NODE-ROUTE-001 종결 내용 일치 확인), 사전/사후 수락 구분, no-response 호출의 수락 시점 완료.

### 3.5 05-relocation-continuity

| ID | 분류 | 요약 |
|---|---|---|
| NODE-RELOC-002 | GAP·상 | §2 참조 — seal 사이 창 메시지 손실 |
| NODE-FOLLOW-001 | GAP·중 | 전달 물량 한도(1,024건/16 MiB) 초과가 `CapacityExceeded`가 아닌 `ActorLocationStale`→`Unavailable`로 종료 — 두 전달 계층 모두. 호출자가 "전달 용량 소진(객체는 정상)"과 "진짜 stale"을 구분 불가. C++ CPP-FOLLOW-001과 동일 계열. 증거: `node/runtime/actors/actor-handoff.ts:863-872, 905-912`, `node/runtime/foundation/service-stateful-runtime.ts:3933-3942` |
| NODE-FOLLOW-002 | GAP·중 | generation 불일치가 `InvalidOperation`이 아닌 `Unavailable`로 종료 — 올바르게 매핑되는 내부 kind(`ActorGenerationStale`)가 존재하나 이 경로에서 미사용. 영원히 성공 못 할 호출을 재시도하게 유도. 증거: `node/runtime/actors/actor-handoff.ts:824-830, 1202-1207`, `node/runtime/framework-errors-internal.ts:72 vs 79` |
| NODE-RELOC-003 | SATISFIED·하 | Host relocation phase와 remote Actor Join phase는 트리거만 다른 같은 stage가 아니다. 전자는 workload aggregate handoff를, 후자는 한 Actor membership transaction을 소유한다. 두 어휘를 합치지 않는 것이 bounded context와 aggregate 불변 조건을 보존한다. |

만족(요약): 수신 슬롯 선확보, 보류 인계·abort 복원, per-Actor 분리, 30초/8-hop/1,024건/16 MiB 상수, 식별자·응답 경로 보존(SHA-256 체크섬 포함), 조건부 원자 소유자 전환(단일 조건부 write), 전환 후 무롤백, idempotent 사후 단계.

### 3.6 06-routing-and-cache

| ID | 분류 | 요약 |
|---|---|---|
| NODE-ROUTE-002 | GAP·중 | ClientServer 선택기가 멤버십 변경(admit/disconnect/remove)마다 **모든 후보의 누적값과 커서를 0으로 리셋** — 스펙은 생존 후보의 누적값 보존을 요구(§5 worked example의 전제). 변경 후 첫 선택이 항상 최대 가중치 서버로 쏠리고 크로스 언어 재현성 상실. RouteMesh 경로는 올바름. 증거: `node/runtime/foundation/service-discovery-registry.ts:181, 200` (rebuild 트리거 `:51, :55, :72, :83`) |
| NODE-ROUTE-003 | GAP·중 | cycle 탐색 상한·fallback 부재: RouteMesh `buildCycle`이 스텝/시간 상한 없는 무한 `for(;;)`, ClientServer는 GCD 축약 없이 `sum(weights)` 스텝을 통째로 사전 계산(가중치 5000/5000 → 10,000-엔트리 배열) — 스펙이 금지한 "가정된 길이" 패턴. 증거: `node/runtime/foundation/service-weighted-selection.ts:68-95`, `node/runtime/foundation/service-discovery-registry.ts:183` |
| NODE-ROUTE-004 | PARTIAL·중 | Spot 이동 통지 무시: `messageFollowReceiver`가 `source.kind !== 'actor'`를 조기 return — spot 종류 레코드에 대해 framework 레벨 resolver 캐시가 무효화되지 않고, wire 레벨 삭제분도 다음 전송 때 stale resolver 캐시에서 재오염(`rememberSpotRoute`). TTL 만료까지 우회 지속. 증거: `node/runtime/host/index.ts:2672`, `node/runtime/foundation/service-stateful-runtime.ts:1795-1806, 4193` |
| NODE-ROUTE-005 | PARTIAL·하 | tiebreak 비교자 불일치: RouteMesh `localeCompare`(ICU) vs ClientServer 코드유닛 `<` — 혼합 케이스/비ASCII RID에서 서로도, ordinal 비교 구현과도 어긋남. 증거: `node/runtime/foundation/service-topology-registry.ts:313-314`, `node/runtime/foundation/service-discovery-registry.ts:178-190` |
| NODE-ROUTE-006 | PARTIAL·하 | `select`마다 N-엔트리 Map clear-and-copy(`replaceState`) — "호출은 커서만 이동" 결정 대비 호출당 O(N). 증거: `node/runtime/foundation/service-weighted-selection.ts:38, 107-113` |

만족(요약): positive 캐시+fence 저장, 미존재/생성중/실패 비캐시, TTL=min(설정, owner lease 잔여), ≥5s 커플링 검증, **수동 피어 admission fence는 올바름(JVM-TOPO-001 계열 없음 — C++과 다름)**, 변경 시점 후보 구축, 직접 지정 대상 불변, publish 스냅샷·단일 레코드/노드.

### 3.7 07-dispatch-loop

| ID | 분류 | 요약 |
|---|---|---|
| NODE-DISP-002 | GAP·상 | §2 참조 — cross-owner HOL 블로킹 |
| NODE-DISP-006 | PARTIAL·중 | wake 방식: mesh 1 ms setTimeout 폴링 + 채널 5 ms 공유 idle waiter — 두 주기가 한 런타임에 공존하고, 스펙이 요구하는 "폴링 주기의 언어 문서 기재(지연 바닥)"가 Node 문서에 없음. 증거: `node/runtime/backend/node/node-raw-mesh-backend.ts:1310-1320`, `node/runtime/channels/channel-receive-loops.ts:612-638` |
| NODE-DISP-007 | PARTIAL·중 | mesh 수신 배치 한도에 경과 시간 축 부재(64건/256파트/1 MiB + 16배치마다 timer yield — 스펙이 금지한 count 프록시). 다중 피어 소켓의 per-peer 계상 부재. 증거: `node/runtime/backend/mesh-dispatch-pump.ts:11, 129-135, 201-205` |
| NODE-DISP-008 | PARTIAL·중 | subscriber/route 수신 루프가 핸들러 완료 포함 디스패치를 인라인 `await` — 해당 소켓의 수신 진행이 핸들러에 결합(channel-request 루프만 동시 디스패치). mesh 경로의 `'protocolError'`는 응답·기록 없이 폐기. 증거: `node/runtime/channels/channel-receive-loops.ts:387-393, 516-522` |

만족(요약): ready set 상태화(중복 진입 불가·residue 재신호), check+enqueue 단일 스팬, claim serial 배타, owner 내 50 ms 시간 예산, rotation cursor, 타이머 공유 heap·3정책 이름 일치·**tick 통계 무한 누적 없음(C++ CPP-TIMER-001과 다름)**, PerActor 타이머 레인.

### 3.8 08-object-lifecycle

| ID | 분류 | 요약 |
|---|---|---|
| NODE-LIFE-001 | GAP·중 | idle sweep이 매 주기 **전체 활성 activation 집합을 동기 루프로 스캔** — 스펙 표준(주기당 최대 64개 + 스캔 위치 이월) 미구현. 대규모 Spot 모집단에서 이벤트 루프 O(N) 점유. 증거: `node/runtime/spots/index.ts:873-931`, `node/runtime/spots/spot-activation-registry.ts:105-107` |
| NODE-LIFE-002 | PARTIAL·중 | 후보 조건을 quiescence+경과 시간만으로 판정하고 점유/relocation 배제는 close 트랜잭션 abort로 사후 처리 — durable `Closing` authority row를 **먼저 발행한 뒤** 로컬 abort하며 발행된 row를 되돌리는 경로가 없음(복구가 materialization의 `closing` 대기 경로에 의존). 증거: `node/runtime/spots/index.ts:888-895`, `node/runtime/spots/spot-activation.ts:739-757` |
| NODE-LIFE-003 | GAP·중 | 일반 Actor message의 `validateActorFence → requireActor`가 `generation !== ref.generation`이면 거부한다. 현재 정식 spec과 Node exact interface는 일반 message의 대상을 `ActorId`로 정하고 current authority를 resolve하며 `ObjectGeneration`을 application message target 조건으로 사용하지 않는다. 따라서 이전의 “spec 판정 필요”는 제거하고 확정 GAP으로 교정한다. Session binding·relocation fence처럼 exact incarnation을 다루는 operation은 이 판정에 포함하지 않는다. 증거: common spec `18-object-routing.ko.md:189-219`, Node exact interface `interfaces/05-actors.ko.md:19-22`, `node/runtime/foundation/service-stateful-runtime.ts:3841-3850`, `node/contracts/Common/ActorRef.ts:3-8` |
| NODE-LIFE-004 | PARTIAL·하 | 공통 값은 계약이 아닌 비교 참조값이다. Node internals에 실제 application 4,096건/16 MiB, lifecycle 1,024건/4 MiB와 고정 byte 비용·owner 시간·burst 회계를 기록했다. 문서 gate와 packed package 기본값 확인이 남아 있다. |
| NODE-LIFE-005 | GAP·상 | 공개 계약인 failure spec §4.4는 `Ready` authority의 owner loss를 Missing과 구분하고 다른 node의 cold activation을 금지한다. Internals 06·08·10은 이를 resolver의 닫힌 결과, activation state와 liveness 책임 분리로 구현하고, 12는 same-target initial recovery root만 허용한다. 현재 resolver는 active authority를 반환할 때 owner lease 만료를 별도 unavailable 결과로 노출하지 않으며, stale route refresh 중 resolver가 `undefined`를 반환하면 곧바로 cold activation으로 전환한다. Authority reserve의 `alreadyExists`가 row가 남아 있는 동안 replacement를 막지만, owner row가 정리되는 시점에도 `Ready owner loss`를 유지하는 별도 판정은 없다. Startup recovery는 같은 target node와 lifecycle의 initial activation으로 제한해야 하며 steady-state owner loss 복구에 사용하면 안 된다. 증거: `common/spec/31-failure-failover-policy.ko.md`, Node exact interface `interfaces/04-spots.ko.md`, `common/internals/06-routing-and-cache.ko.md`, `common/internals/08-object-lifecycle.ko.md`, `common/internals/10-liveness-and-state.ko.md`, `common/internals/12-service-wire-protocol.ko.md`, `node/runtime/locations/resolvers.ts:744-820`, `node/runtime/host/spot-address-transport.ts:535-556`, `node/runtime/host/instance-activation-authority.ts:117-170` |

만족(요약): 폐쇄 kind enum·이종 재등록 거부, Entry Spot 이동 단위 제외(Entry 거주 Actor는 이동), 일반 메시지 비생성, factory 1회(store CAS·패자 대기), 실패 생성 즉시 abort+주기 sweep, `IdleEvicted` 콜백 후 위치 해제(**C++ CPP-LIFE-002와 달리 순서 올바름**), 이중 축 레인·relocation 보류 상수, 양 지점 capacity ceiling.

### 3.9 09-session-binding

| ID | 분류 | 요약 |
|---|---|---|
| NODE-SESS-001 | GAP·상 | §2 참조 — 크로스 노드 스왑 tombstone 부재 |
| NODE-SESS-002 | GAP·상 | §2 참조 — 하트비트가 애플리케이션 레인에 |
| NODE-SESS-003 | SATISFIED·하 | 세 primitive는 같은 serial queue의 중복 구현이 아니다. `ServiceMailbox`는 owner claim·relocation seal·byte admission을, `EventLoopWorkQueues`는 process infrastructure/application 진행 분리를, `RouterOperationQueue`는 native request slot release 순서를 소유한다. 공통 base는 세 aggregate의 상태와 시간 규칙을 한 interface에 노출하는 얕은 abstraction이 되므로 추가하지 않는다. |
| NODE-SESS-004 | PARTIAL·중 | 새 bind가 terminal이 될 때까지 기존 native·logical binding을 유지하고, 성공 뒤 route owner를 원자 교체한다. 새 bind 실패는 기존 binding을 변경하지 않으며, bind 성공 뒤 remote confirmation 실패는 새 binding을 제거하고 이전 binding과 confirmation을 복원한다. Packed package와 실제 reconnect process gate가 남아 있다. |

만족(요약): 세션별 직렬 executor(동일 연결 콜백 비동시), 연결 identity 3요소 쌍, generation fence 필터, 재연결 fresh(복원 경로 없음), 이동 시 연결 유지·라우트만 in-place 갱신.

### 3.10 10-liveness-and-state

| ID | 분류 | 요약 |
|---|---|---|
| NODE-OBS-001 | PARTIAL·하 | terminal 보관 초과 시 **최신을 버리고 최초를 유지** — 스펙은 "가장 오래된 것부터 버림". 놓친 구독자가 최신이 아닌 최초 terminal을 봄(discard 카운트로 손실은 노출). 증거: `node/runtime/diagnostics/topology-runtime-projections.ts:349-383` |
| NODE-OBS-002 | PARTIAL·중 | Off이고 ambient flow가 없는 hot path는 객체 생성과 `AsyncLocalStorage.run` 없이 callback을 직접 실행한다. Parent flow를 지워야 하는 nested 경로만 storage 경계를 사용한다. Enabled flow는 첫 transition의 mode를 flow 수명 동안 유지하고 flow 밖에서는 live mode 변경을 즉시 반영한다. Packed package benchmark만 남아 있다. |

만족(요약): 5s/15s 단일 상수·설정 비노출(옵션 파라미터는 테스트 전용), 비즈니스 메시지 기한 비연장(fanout의 전체 레코드 연장은 스펙 29 §4 명시 사항), 체크 신호 앱 미도달, 7-state 폐쇄 집합, bind 후 endpoint 해석, 관찰 비차단·coalescing·구독 비절단, 언바운드 라벨 없음 — 12개 문서 중 가장 충실(C++과 동일 경향).

### 3.11 11-message-ownership

| ID | 분류 | 요약 |
|---|---|---|
| NODE-OWN-001 | GAP·상 | §2 참조 — content-type 미사용 + 무음 텍스트 fallback |
| NODE-OWN-002 | GAP·중 | 형식을 test-parse로 판별: 확장 serializer 등록 시 `looksLikeJson()` + 전체 페이로드 `JSON.parse` 시도 후 실패하면 등록 serializer에 전달 — 헤더가 아닌 본문 파싱으로 형식 발견, 실패 파싱 비용(전체 UTF-8 변환+예외)을 메시지당 지불. 증거: `node/runtime/messaging/payload-codec.ts:97-107, 223-256` |
| NODE-OWN-003 | PARTIAL·중 | Public payload의 defensive copy는 immutable 계약으로 유지한다. Runtime-owned `Buffer` 인수와 JSON lazy decode는 내부 borrowed storage를 사용해 생성·빈 길이 확인·문자열 변환의 중복 복사를 제거했다. Packed package·clean consumer evidence가 남아 있다. |
| NODE-OWN-004 | PARTIAL·중 | Completion·managed-stream·routed join의 연속 복사를 단일 ownership transfer 또는 snapshot으로 줄였다. Raw mailbox는 native part 복사 직후 claimed 원본 envelope 참조를 해제하므로 handler 실행 중 두 표현을 함께 보관하지 않는다. 대형 multipart process retained-byte benchmark가 남아 있다. |
| NODE-OWN-005 | PARTIAL·하 | Handler의 lazy decoder는 runtime에서 인수한 encoded payload만 capture하고 원본 `Message`를 보관하지 않는다. Packed package handler 수명 evidence가 남아 있다. |
| NODE-OWN-006 | PARTIAL·하 | 송신 선택이 business type별 캐시 없이 메시지마다 `canSerialize` 전체 스캔 재실행(할당은 없음). 증거: `node/runtime/messaging/payload-codec.ts:184, 191-221` |

만족(요약): header 우선·미admit 메시지 비역직렬화(byte 계상만), 실행 권한 이후 lazy decode, 큐 통과 무복사(retain=identity), 이동 레코드 seal 시점 구축(**C++ CPP-OWN-002와 달리 hot path 비용 없음**), 채널 경로의 content-type 조회+`ProtocolError`, 송신 주 경로 무왕복.

### 3.12 12-service-wire-protocol

| ID | 분류 | 요약 |
|---|---|---|
| NODE-WIRE-001 | GAP·상 | §2 참조 — RouteMesh 금지 상한 잔존 + ClientServer 협상값 변경 가능 |
| NODE-STREAM-SIZE-001 | GAP·중 | StreamNode의 64 KiB 기본값, configurable inbound C→S 검사, handler 미전달과 peer disconnect는 구현됐지만, 초과를 일반 `Error('STREAM frame exceeds MaxMessageSize.')`로 처리하고 server 진단 sink에 `EMSGSIZE`를 기록하지 않는다. `handleMalformedFrame`은 disconnect 실패만 `onError`에 보내며 size rejection 자체를 진단 trace로 남기지 않는다. 증거: common spec `19-stream-session.ko.md:182-188`, Node exact interface `interfaces/06-stream-worker.ko.md:105-111`, `node/runtime/streams/stream-frame-reassembler.ts:72-85,128-140`, `node/runtime/streams/stream-session-runtime.ts:1003-1011,1057-1065`, `framework/languages/node/test/contract/stream-session-runtime.test.js:937-970` |
| NODE-WIRE-002 | GAP·상 | DEC-05 후 `framework-json-v1`은 internals 권고가 아니라 public codec contract다. 그러나 프로파일 명칭이 Node package에 없고, 중복 속성 last-wins, 64-bit 정수 형식·범위 미검증, `NaN`→`null`, typed byte array의 padded base64 처리 부재가 남아 있다. Golden fixture는 Node를 consumer로 명시하지만 Node test가 이를 실행하지 않는다. 증거: common public spec `04-message-model.ko.md` codec profile, `node/runtime/channels/channel-envelope.ts:363-366, 405-412`, `framework/runtime/protocol/golden/framework-json-v1.json:1-25`; `framework/languages/node/test/`에서 fixture 참조 없음 |
| NODE-WIRE-003 | GAP·중 | admission 레코드에 maintenance wave 필드 자체가 없음(descriptor·인코더·디코더) — 스펙 §4는 `update`로 변경 가능해야 한다고 규정. 증거: `node/runtime/foundation/service-topology-registry.ts:20-45`, `node/runtime/foundation/service-wire-m6a-codec.ts:251-300` |
| NODE-WIRE-004 | PARTIAL·중 | descriptor revision 위반(하위 rev, 동일 rev 다른 bytes)이 `ServiceWireProtocolError`가 아닌 무음 stale 거부로 강등(RouteMesh는 전 케이스, ClientServer는 하위 rev 케이스). 순서 검증 자체는 올바름. 증거: `node/runtime/foundation/raw-service-mesh-runtime.ts:1198-1215`, `node/runtime/channels/channel-socket-registry.ts:1313-1321` |
| NODE-WIRE-005 | PARTIAL·중 | authority key: **encode는 완전 준수**(`zla1:` — C++ CPP-WIRE-001과 달리 올바름)이나 `decodeAuthorityKey`가 스키마 요구 검증(1..255 바이트 한도, 길이 필드 leading-zero 거부, UTF-8 유효성, 776-byte 캡)을 생략, 비정규 escape가 비멱등 왕복. `authority-key-v1.json` fixture를 소비하는 Node 테스트 없음. 증거: `node/runtime/locations/authority-key-codec.ts:6-45` |
| NODE-WIRE-006 | PARTIAL·중 | RelocationId: `randomUUID()`(CSPRNG이나 122-bit) + **충돌 재생성 없음** + `targetAttemptGeneration` 전 지점 `1n` 고정(대상 교체 미구현). 증거: `node/runtime/host/service-relocation-host-runtime.ts:772, 894, 1035` |
| NODE-WIRE-007 | PARTIAL·중 | OperationId 비영(非零) 강제 없음(디코드도 `u64` 사용, `nonZeroU64` 미사용), ReplyRouteId 고갈 가드가 도달 불가 dead code(`0n`에서 증가하는 bigint는 `<= 0n` 불가). 증거: `node/runtime/actors/actor-context.ts:468-474`, `node/runtime/actors/actor-handoff.ts:665-678` |
| NODE-WIRE-008 | PARTIAL·하 | messageFollow 무효화가 `directActorRoutes`만 삭제하고 두 번째 캐시 `actorRoutes`는 stale 잔존(같은 핸들러의 spot 분기는 양쪽 삭제 — 누락이 자명). 증거: `node/runtime/locations/resolvers.ts:568-576, 629-639` |
| NODE-WIRE-009 | PARTIAL·하 | ClientServer client와 server는 stale·duplicate liveness ACK를 readiness에 적용하지 않고 typed protocol 진단을 runtime failure sink에 게시한다. Packed reconnect process에서 진단 cardinality 확인이 남아 있다. |

만족(요약): StreamNode의 초과 message는 handler에 전달하지 않고 peer를 disconnect한다(EMSGSIZE 진단만 gap). 그 밖에 생성 상수 byte-identical 소비, frame prefix·디코드 검증·canonical UTF-8·trailing 거부, metadata 1,024 캡, multipart 프로파일 정확, HWM에 body part 크기만 계상(**C++ CPP-WIRE-007과 달리 올바름**), liveness 프로토콜 전항목, fanout beacon 전항목, admission 방향성·topologyKind 분리, messageFollow 와이어 검증 전항목, authority key encode 및 전체 read/write 경로 일원화, userSpotCreate/Close envelope.

---

## 4. 검증 증거와 남은 gate

이 보고서의 판정은 현재 source와 생성된 declaration을 정적으로 비교한 결과다. 생성된
`dist` package도 observer·sink, 기존 diagnostics enum, object 개별 조회 누락과 STREAM send timeout
누락을 source와 동일하게 노출한다. 따라서 이 항목들은 source만의 잠재적 차이가 아니라
현재 생성 package의 실제 public contract gap이다.

현재 test는 일부 오래된 동작을 오히려 고정한다.

- `contract-surface.test.js` 위치 조회는 `listObjectLocations` 존재만 확인하고
  `findActorLocation`, `findSpotLocation`, `unavailable` state를 검증하지 않는다.
- 같은 surface test는 `ZLinkSessionSendCall.submit(...)`은 확인하지만 call별 `timeout(...)`을
  요구하지 않는다.
- `channel-client.test.js`, `nestjs-module.test.js`는 제거 대상인
  `setMessageFlowObserver(...)`를 정상 public 경로로 사용한다.
- Timer test는 `maxCatchUpTicks: 0`과 policy tick 순서만 검증하고 소수·무한대·`INT_MAX`
  초과 startup 거부를 검증하지 않는다.

이러한 test가 통과하더라도 최신 formal spec 준수 증거가 되지 않는다. 다음 증거는 아직
open이며 정적 판정과 분리해 닫아야 한다.

이 개정에서 실행한 범위는 다음과 같다.

| Gate | 결과 | 판정 범위 |
|---|---|---|
| `npm run typecheck` | PASS, exit `0` | 현재 source가 TypeScript 정적 검사를 통과한다. Spec compliance를 증명하지 않는다. |
| `node --test test/contract/contract-surface.test.js` | PASS, 38/38 | 현재 생성 declaration과 기존 assertion이 일치한다. 위에 열거한 최신 exact interface 누락을 assertion하지 않으므로 compliance 증거가 아니다. |
| full runtime·coverage·CI gate | NOT RUN | 이 개정은 문서 범위 정적 리뷰이므로 aggregate 실행을 완료 판정에 사용하지 않는다. |
| packed tarball·clean consumer | NOT RUN | 새 public declaration을 구현한 후 package provenance와 함께 다시 확인해야 한다. |
| multi-process·cross-language E2E | NOT RUN | DEC-01·03·05·06·08·09·17의 runtime 완료 판정은 아직 open이다. |

| 항목 | 확인 방법 |
|---|---|
| Decision 반영 public surface | 새 선언으로 다시 build한 뒤 `contract-surface.test.js`와 `verify_packaged_contract.sh`에서 source·generated declaration·packed tarball을 함께 검증 |
| DEC-01 provider 대체 경로 | 제거 전 다섯 언어 logger provider process E2E로 `zlink.message_flow`, `zlink.dispatch_error`, provider failure 격리를 검증 |
| DEC-03 CPU worker saturation | CPU worker를 포화시킨 뒤 이미 admission된 async I/O의 완료·timeout·shutdown이 계속 진행하는지 process test |
| DEC-05 JSON profile | 실제 packed Node package가 `framework-json-v1.json`의 golden·reject fixture를 다른 언어와 양방향으로 소비하는지 검증 |
| DEC-06 Server-only ClientServer | Server role만 등록한 실제 host에서 send/request가 local handler를 실행하지 않고 `NotConfigured`로 끝나는지 검증 |
| DEC-08 object query | Missing·Creating·Ready·Unavailable·Store failure matrix, page size `1..1000`, encoded page 4 MiB와 opaque continuation을 Store fixture로 검증 |
| DEC-09 STREAM send timeout | 같은 session에 서로 다른 deadline의 send를 제출해 timeout terminal-once·no-replay·나머지 ordering을 실제 process로 검증 |
| DEC-17 participant state limit | 64 MiB 이하의 checksum·logical length 보존과 64 MiB+1 byte의 `Blocked/StateIncompatible`·source authority 유지를 Store·process E2E로 검증 |
| 핸들러 정지 중 타임아웃/shutdown/신규 피어 진행 (03) | 전 핸들러를 미해결 promise에 park하는 런타임 실험 |
| seal 사이 창의 메시지 손실 실측 (05, NODE-RELOC-002) | 장시간 turn + 동시 request + relocation 트리거 테스트 (`test/m6c` 기반) |
| 1 ms/5 ms 폴링 바닥의 end-to-end 지연 가시성 (07) | 지연 측정 또는 바인딩 즉시-wake 가능성 검토 |
| router 소켓 내 per-peer 공정성 (07) | core 바인딩 검토 또는 2-피어 포화 실험 |
| 가중치 비율의 크로스 언어 수렴 (06) | 크로스 언어 E2E 분포 비교 |
| `B,A,B,B` 리터럴 시퀀스 (06) | 두 선택기 대상 시퀀스 어서션 유닛 테스트 |
| 빈 페이로드 적치 시 count 축 우선 도달 (08) | 레인 count 포화 테스트(byte 축은 기존 테스트 있음) |
| 로컬 abort된 idle 축출 후 durable Closing row 복구 (08, NODE-LIFE-002) | 후보 판정과 close 사이 점유 주입 테스트 |
| Ready Instance owner loss (08, NODE-LIFE-005) | owner process 종료와 lease 만료 뒤 resolver가 Missing 대신 unavailable을 반환하고, 새 factory·handler·queue recovery 없이 call이 bounded `Unavailable`로 끝나는 process E2E. Initial cold activation same-target recovery는 별도 scenario로 검증 |
| §7–§10 durable authority/relocation CAS 불변식 (12) | 현재 package로 M6C contract·E2E 스위트를 다시 실행하고 정확한 버전·test 수·결과를 기록 |
| 느린 관찰자에도 처리 속도 유지 (10) | 관찰자 지연 주입 벤치마크 |
| 바인딩 강제 복사 기준선 (11) | native 바인딩/core 프로파일링 |

---

## 5. 크로스 언어 비교의 판정 경계

다른 언어 구현은 수정 방식을 비교하는 참조일 뿐 Node 계약의 출처가 아니다. 이전 버전은
C++ 보고서의 항목 ID와 판정을 직접 복제했기 때문에, C++ 보고서가 다시 검토되면 Node
보고서의 사실까지 낡아 보일 수 있었다. 따라서 상대 언어 ID 대조표를 제거했다.

현재 Node에서 확인한 언어 공통 검증 주제는 다음과 같다.

- `framework-json-v1` golden·reject fixture의 다섯 언어 양방향 호환성
- RouteMesh의 금지된 message-size field 부재, ClientServer negotiated bound와 participant state 64 MiB 경계
- Message Follow의 terminal error·cache generation 안전 조건
- 세션 교체·logical disconnect·relocation route update의 exact binding identity
- public exact surface와 실제 packed package declaration의 일치

이 주제는 공통 spec과 언어별 exact interface로 판정하고, 다른 언어에서도 같은 결함이
보인다는 이유만으로 Node gap을 닫지 않는다.

---

## 6. 권고

1. **이 보고서에서 unique ID 관리** — 정식 spec에 구현 진행 기록을 다시 넣지
   않고, 이 plan 문서가 33 GAP과 29 PARTIAL의 상태·완료 증거를 소유한다. 항목을
   수정할 때는 source 변경만으로 닫지 않고 해당 contract·package·process 증거를 같은 ID에
   기록한다.
2. **수정 순서 제안**:
   - 1차 (public contract·visible failure): `NODE-CONTRACT-DIAG-002`의 provider 선행 E2E,
     `NODE-CONTRACT-STREAM-001`, `NODE-CONTRACT-CLIENTSERVER-001`, `NODE-CONTRACT-RELOC-001`,
     NODE-DISP-001, NODE-RELOC-002, NODE-SESS-002
   - 2차 (public surface·codec): `NODE-CONTRACT-DIAG-001`, `NODE-CONTRACT-LOC-001`,
     `NODE-CONTRACT-TIMER-001`, NODE-WIRE-001, NODE-WIRE-002
   - 3차 (runtime 의미): NODE-DISP-002, NODE-COMP-001, NODE-EXEC-001, NODE-LAYER-001,
     NODE-LIFE-003, NODE-LIFE-005, NODE-SESS-001, NODE-FOLLOW-001/002, NODE-OWN-001
   - 4차 (상호운용·프로토콜): NODE-WIRE-003, NODE-ROUTE-002/003과 나머지 protocol PARTIAL
   - 5차 (자원·구조): NODE-LIFE-001(idle sweep 상한), NODE-OWN-003(접근자 복사), 나머지 PARTIAL
3. **남은 설계 판정은 NODE-RELOC-003 한 건**이다. Remote join/transfer와 host relocation이
   internals 05의 “하나의 상태 전이 규칙”에서 같은 operation domain인지 확정한 뒤 구현
   GAP 또는 정상 분리로 닫는다. NODE-LIFE-003은 formal spec이 이미 결론을 제공하므로
   더 이상 설계 판정 항목이 아니다.
4. **완료 증거를 분리한다**. Typecheck·unit·contract test, packed tarball·clean consumer,
   aggregate CI, 실제 multi-process·cross-language E2E를 따로 기록한다. 한 범주의 통과로 다른
   범주의 gap을 닫지 않는다.
