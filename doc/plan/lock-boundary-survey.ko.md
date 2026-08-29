# lock/동기 경계 조사 보고서

> 작성: codex gpt-5.6-sol(ultra) 조사 에이전트, 검수: Claude 감독관 (2026-08-29).
> 다음 단계(회수 구현)는 이 문서의 우선순위표를 따르되 사용자 승인 후 착수한다.

조사 결과, 단순 bridge 삭제만으로 회수 가능한 범위는 작습니다. C++의 같은 phase 조회 통합이 가장 큰 동작 보존 후보이고, .NET 원격 Actor 경로·Java wrapper·relocation callback은 대부분 owner 또는 placeholder를 다시 설계해야 합니다.

기준은 [계획 §0.6](/home/hep7/project/zlink/doc/plan/implementation-plan.ko.md:150), [§9-③](/home/hep7/project/zlink/doc/plan/implementation-plan.ko.md:397), [부록 A](/home/hep7/project/zlink/doc/plan/implementation-plan.ko.md:407)와 다음 계약입니다.

- C2 상태는 하나의 state lane이 소유합니다. [스펙 06 §4](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:93)
- 공개 동기 bridge는 스펙의 세 조건을 모두 충족해야 합니다. [스펙 06 §5](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:212)
- 외부 callback은 turn A claim → lane 밖 callback → turn B 정산으로 나눕니다. [스펙 06 §6 유형 ③](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:310)
- 한 메시지의 조회는 한 turn의 immutable snapshot으로 묶습니다. [스펙 07 §7](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md:493)

## 언어별 계수

| 언어 | 현재 계수 | 분류 |
|---|---:|---|
| .NET 제품 bridge | **550** | 핫패스 16 / 콜드 142 / 공개 동기 계약 최소 6 / 기타·미확정 386 |
| .NET 전체 실행 코드 | **567** | 제품 550 + test/sample/bench 17 |
| C++ literal `run().get()` | 제품 990 / 전체 **1,009** | test 19 포함 |
| C++ helper 전개 effective | 제품 **1,060** / 전체 **1,110** | 핫패스 15 / 콜드 383 / 공개 동기 계약 0 / 기타·미확정 662 / test 50 |
| Java binding wrapper | **30** | 직접 제거 가능 0 / 유지 28 / `[의심]` 2 |
| .NET `_barrierGate` | **0** | `RunBarrierState` semantic caller 16으로 이미 이관 |

`기타·미확정`은 콜드패스가 아닙니다. 일반 runtime/message 경로와 여러 문맥이 섞인 helper를 `(b)`로 과대 분류하지 않고 `[의심]`으로 남긴 값입니다.

---

## .NET `AwaitStateLane`

### 재계수

계획서의 664는 callsite 수가 아니라 `AwaitStateLane` 식별자 출현 수입니다.

| 항목 | 수 |
|---|---:|
| `AwaitStateLane` 식별자 | 664 |
| helper 선언 | -135 |
| named helper callsite | **529** |
| 전체 tree `GetAwaiter().GetResult()` 구조식 | 175 |
| helper 본문과 중복 | -135 |
| test/e2e 문자열 | -2 |
| 직접 실행 bridge | **38** |
| 전체 실행 callsite union | **567** |
| 제품 callsite union | **550** |

제품 밖 실행 bridge는 test 11, sample 4, benchmark 2입니다.

제품 550의 엄격한 분류는 다음과 같습니다.

| 분류 | 수 | 대표 근거 |
|---|---:|---|
| (a) 원격 Actor 정상 수신 hot path | **16** | [Mesh pump](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:174), [AJQ](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:233), [Handoff](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:347) |
| (b) 명백한 init/stop/dispose/relocation | **142** | [Actor handoff lifecycle](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:975), [Spot retire](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotRetireTransport.cs:2623) |
| (c) 공개 동기 계약의 최소 bridge | **6** | `Connect`/`Disconnect`/`ListConnections` 3곳, `IsDisposed`, Fanout/ClientServer `GetStatus` |
| (d) 기타·혼합 `[의심]` | **386** | 일반 runtime, stream, channel, service helper |

공개 동기 표면에 귀속되는 bridge는 8곳이지만, [EndpointConnections의 실패 rollback](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Configuration/ZLinkEndpointConnections.cs:36) 두 곳은 외곽 공개 계약 자체가 요구하는 최소 bridge가 아닙니다. 따라서 엄격한 `(c)`는 6이며, 두 rollback bridge는 `(d)` 회수 후보로 돌렸습니다.

### 원격 Actor 1회당 동적 횟수

고정 조건은 warm Actor, 활성 User Spot, `SpotWide`, current direct route, relocation/Message Follow 없음, bound Session 아님, AJQ 즉시 획득입니다.

| 단계 | 정적 site | 실제 통과 |
|---|---:|---:|
| ready signal/batch claim | 2 | 2 |
| AJQ acquire/queued/release | 3 | 3 |
| runtime admission | 1 | 1 |
| task runner/supervisor | 2 | 2 |
| Spot claim/lane lookup | 2 | 2 |
| Actor registry | 1 | **3** |
| Handoff capture/route/block | 3 | **5** |
| Actor handler owner | 1 | 1 |
| 별도 DI handler 생성 | 1 | 조건부 1 |
| 합계 | 16 | **19 / 20** |

따라서 send와 request 모두 다음과 같습니다.

- native callback 시작 → application handler: **19회**
- 별도 DI handler 사용: **20회**
- `EnqueueActor` 이후 message-local 구간: **16/17회**

request 전용 bridge는 이 구간에 없습니다.

주요 회수 후보는 다음과 같습니다.

- Actor registry 3회 → dispatch projection 1회: **-2**, generation/replacement fence를 포함한 리팩터링.
- Handoff 5회 → ingress projection·capture claim 통합: **-3~-4**, arrival ordering 때문에 의미 리팩터링 필요.
- Spot claim과 Actor lane lookup 통합: **-1**, relocation barrier와 lazy queue 생성을 같은 turn에서 처리.
- `RuntimeTaskRunner`가 lane 안에서 supervisor의 다른 lane을 기다리는 구조: **-1**, `[의심-H]`; 단일 admission owner가 필요합니다.
- [ScopedHandlerInstanceOwner](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Handlers/ZLinkScopedHandlerInstanceOwner.cs:25)는 lane 안에서 DI 생성자를 호출합니다. **조건부 -1**, 재진입 위험도 함께 해소해야 합니다.
- AJQ acquire/queued/release는 서로 다른 permit 전이입니다. 단순 통합 대상이 아닙니다.

---

## C++ state-lane `.get()`

### 재계수

| 범위 | literal | helper 전개 effective |
|---|---:|---:|
| 제품 source/header | **990** | **1,060** |
| test | 19 | 50 |
| 전체 | **1,009** | **1,110** |

Effective 계수는 다음을 포함합니다.

- `state_lane_t::run(...).get()` 1,009곳
- 분리형 `future = run(...); future.get()` test 2곳
- [actor_gateway_state_t::sync](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.hpp:184) 정의를 실제 호출 100곳으로 치환

계획의 과거 456 산출 범위가 남아 있지 않습니다. 따라서 비교는 다음처럼 범위를 나누어야 합니다.

- 전체 literal 기준: `456 → 1,009`
- 제품 literal 기준: `456 → 990`
- helper까지 전개한 현재 실행 경계: 제품 **1,060**

`456`과 어느 값이 정확히 같은 scope인지는 `[의심]`입니다. 증가분 전체를 회귀라고 판정할 수 없습니다.

제품 effective 1,060의 분류입니다.

| 분류 | 수 |
|---|---:|
| (a) 원격 Actor hot-path static site | **15** |
| (b) 명백한 lifecycle/cold path | **383** |
| (c) 공개 동기 계약상 불가피함이 입증된 곳 | **0** |
| (d) 기타 runtime/message 경로 `[의심]` | **662** |

상위 파일은 [spot_runtime.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:13030) 243, `public_host_runtime.cpp` 115, `mesh_node_runtime.cpp` 72, `raw_client_server_owner.cpp` 57, `stateful_object_runtime.cpp` 56, `raw_mesh_node_owner.cpp` 47, [actor_transfer_coordinator.cpp](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/actor_transfer_coordinator.cpp:609) 45입니다.

### 원격 Actor 1회당 동적 횟수

현재 코드에서 직접 보이는 wait는 send/request 9/11입니다. helper를 호출 지점에서 전개하면 **17/19**입니다.

| phase | send | request | 대표 위치 |
|---|---:|---:|---|
| host/coordinator·dispatch admission | 10 | 11 | [host precheck](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:6094), [Actor projection](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:13030), [gateway context](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:2255) |
| relay/fence/backlog admission | 5 | 5 | [retiring/fence](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10240), [second fence](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10352) |
| warm materialization·dispatch projection | 1 | 1 | [spot_runtime.cpp:10554](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10554) |
| request bookkeeping | 0 | 1 | [spot_runtime.cpp:10829](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10829) |
| callback admission | 1 | 1 | [spot_runtime.cpp:4111](/home/hep7/project/zlink/framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:4111) |
| 합계 | **17** | **19** | |

부록 A의 11/13보다 6회 많지만, 현재 직접 wait는 오히려 2회 줄었습니다. 과거 조사에서 coordinator/gateway 내부의 hidden wait 8회를 전개하지 않았기 때문에 생긴 차이입니다.

감축 후보는 다음과 같습니다.

| 후보 | 예상 감소 | 난이도 | 판정 |
|---|---:|---|---|
| coordinator phase별 projection 통합 | 양쪽 **-3** | 중간 | phase 경계를 유지하면 동작 보존 가능 |
| node dispatch 4→1, relay pre-fence 2→1 | 양쪽 **-4** | 중간~높음 | 두 번째 fence를 제외하면 동작 보존 후보 |
| gateway `actor_context` immutable projection | 양쪽 -1 | 중간~높음 | owner 리팩터링 |
| callback admission을 Spot serial owner로 이관 | 양쪽 -1 | 높음 | lifecycle 리팩터링 |
| request reservation/counter를 기존 claim에 포함 | request -2 | 높음 | reservation 정산 리팩터링 |
| unified admission token | 추가 -2~-3 | 매우 높음 | 목표 5~6에 필요한 의미 재설계 |

같은 phase의 projection만 합치면 **17/19 → 10/12**입니다. Gateway, callback, request bookkeeping까지 바꾸어도 약 **8/8**입니다. 목표 5~6에는 node/coordinator ownership 또는 admission token 통합이 필요합니다.

두 번째 authority fence는 제거 후보가 아닙니다. [계획의 유지 판정](/home/hep7/project/zlink/doc/plan/implementation-plan.ko.md:98)과 현재 liveness 설명이 일치합니다.

---

## Java binding wrapper 전수 판정

현재 wrapper는 Dealer 8, Router 17, Subscriber 5로 정확히 30곳입니다. 직접 위임 가능 판정은 **0곳**입니다.

Wrapper monitor는 native call 진입만 직렬화합니다. async send/request 완료까지 소유하지 않고 generation·identity claim이나 owner assertion도 없습니다. 따라서 최종 protocol owner는 아니지만, monitor만 제거하면 기존 call-entry 순서가 달라집니다.

### Dealer

| 위치 | operation | 직접 위임 | 근거 |
|---|---|---|---|
| [D:27](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:27) | bind | 아니오 | production caller 부재는 owner 계약이 아님 |
| [D:28](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:28) | connect | 아니오 | public/manual/location/reconnect producer 병존 |
| [D:29](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:29) | disconnect | 아니오 | public 호출과 liveness reconnect가 공유 |
| [D:31](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:31) | receiveFlow | `[의심]` | `applyLock`과 close join은 있으나 send/recv/connect와의 동시성 계약이 없음 |
| [D:39](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:39) | send | 아니오 | application과 liveness/control submit 공유 |
| [D:44](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:44) | request | 아니오 | application, manual admission, location monitor 공유 |
| [D:52](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:52) | recv | 아니오 | receive loop는 하나지만 reconnect/close owner와 결합되지 않음 |
| [D:62](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDealerSocket.java:62) | close | 아니오 | submit, monitor, reconnect와 teardown 경합 |

### Router

| 위치 | operation | 직접 위임 | 근거 |
|---|---|---|---|
| [R:28](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:28) | bind | 아니오 | bind 전에 flow/public topology에 노출 |
| [R:29](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:29) | connect | 아니오 | public endpoint와 background auto-connect 병존 |
| [R:30](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:30) | disconnect | 아니오 | public/background producer 병존 |
| [R:32](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:32) | receiveFlow | `[의심]` | Dealer와 같은 단일 setter 후보이나 교차-operation 계약 부재 |
| [R:36](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:36) | connect RID | 아니오 | auto-connect gate가 public connect를 덮지 않음 |
| [R:37](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:37) | probe | 아니오 | RID/probe/connect transaction owner가 불완전 |
| [R:38](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:38) | max size get | 아니오 | public/receive/descriptor producer 공유 |
| [R:39](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:39) | max size set | 아니오 | bootstrap 뒤에도 public mutation 가능 |
| [R:40](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:40) | weight get | 아니오 | public read와 descriptor projection 병존 |
| [R:41](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:41) | weight set | 아니오 | bootstrap와 public setter 병존 |
| [R:42](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:42) | last endpoint | 아니오 | active socket/close와 공유, owner assertion 없음 |
| [R:48](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:48) | recv | 아니오 | receive producer는 하나지만 topology/flow/close가 같은 raw object 공유 |
| [R:58](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:58) | send | 아니오 | application과 control/liveness submit 병존 |
| [R:65](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:65) | request | 아니오 | application/internal request producer 병존 |
| [R:76](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:76) | reply | 아니오 | 여러 handler completion thread와 wrapper 우회 reply 존재 |
| [R:80](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:80) | disconnect peer | 아니오 | receive rejection과 scheduled liveness 공유 |
| [R:84](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRouterSocket.java:84) | close | 아니오 | retained option과 이미 시작된 reply를 금지하는 join 없음 |

### Subscriber

| 위치 | operation | 직접 위임 | 근거 |
|---|---|---|---|
| [S:23](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSubscriberSocket.java:23) | bind | 아니오 | caller 부재만으로 owner를 증명할 수 없음 |
| [S:24](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSubscriberSocket.java:24) | connect | 아니오 | public request와 location reconciler 공유 |
| [S:25](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSubscriberSocket.java:25) | disconnect | 아니오 | manual/location teardown 병존 |
| [S:33](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSubscriberSocket.java:33) | subscribe | 아니오 | admitted receive tick과 monitor-driven close의 join 부재 |
| [S:44](/home/hep7/project/zlink/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaSubscriberSocket.java:44) | close | 아니오 | tick, monitor, stop/expiry producer 병존 |

Application-message hot wrapper는 Dealer send/request, Router recv/send/request/reply, Subscriber subscribe의 7곳입니다.

---

## .NET relocation C2 잔여

계획의 과거 `_barrierGate` 16곳은 이미 state lane으로 이관되었습니다.

- `_barrierGate|barrierGate|BarrierGate`: 저장소 전체 **0건**
- `RunBarrierState`: 정의·forwarder를 제외한 semantic caller **16곳**
- Spot admission open은 이미 turn A → callback outside → turn B입니다: [turn A](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotSerialExecutor.cs:911), [callback](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotSerialExecutor.cs:933), [turn B](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotSerialExecutor.cs:1200).
- Canonical replay reservation, join prewarm, retire abort cleanup도 이미 유형 ③ 구조입니다.

따라서 [계획 102행](/home/hep7/project/zlink/doc/plan/implementation-plan.ko.md:102)의 “16곳 이월”은 현재 코드와 맞지 않고, [108행](/home/hep7/project/zlink/doc/plan/implementation-plan.ko.md:108)이 현재 상태와 일치합니다. `[의심-doc]`입니다.

현재 재설계 대상은 다음과 같습니다.

| 심각도 | 대상 | callback/effect 위치 | 현재 구조와 필요한 변경 |
|---|---|---|---|
| H | Handoff `prepareCapture` | [정의·호출](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:342), callers: `ZLinkActorHandoffIngress:64,172`, `ZLinkActorInboundPipeline:313,475` | callback이 Handoff lane 안에서 다른 lane과 runtime 등록에 진입. arrival/capacity placeholder → callback outside → commit/failure 정산 필요 |
| H | Handoff Cancel/Dispose/lease | [token Dispose](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:65), `:975`, `:1059`, `:1790`, `:1871` | `Cancel`/`Dispose`가 동기 callback을 실행할 수 있음. turn A에서 handle을 떼고 밖에서 정리한 뒤 identity 재검증 필요 |
| M | Handoff diagnostic listener | `HandoffState:151,163,317,362,440,517,665,938,1077,1161,1855` | 외부 `Action<string>` 11곳이 lane 안에서 실행. non-throwing 계약이 없으므로 예외 의미는 `[의심]` |
| H | Standalone `AttemptSlot.RunAsync` | [lane이 외부 async 전체를 await](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkStandaloneActorRelocationRuntime.cs:2998), callers `:1040,1070,1129,1344,2362,2488,2547` | store I/O, rollback, 다른 Handoff lane을 turn 전체에서 기다림. attempt generation placeholder와 turn B CAS 정산 필요 |
| H | async serial seal reservation | [callback under `_admissionGate`](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:1130), production callback [timer freeze](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotActivationExecution.cs:1194) | queue monitor 안에서 외부 timer owner 호출. queue-boundary placeholder 뒤 callback outside, 결과를 seal에 정산해야 함 |
| L·`[의심]` | sync serial seal `admit` | [admit under `_admissionGate`](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:493) | 유형 ③ 위반 형태이나 callback overload의 production caller는 찾지 못함. test와 latent internal API는 존재 |

---

## 회수 우선순위

순위는 hot operation 한 번에서 줄어드는 bridge 수와 구현 난이도를 함께 반영했습니다.

| 순위 | 대상 | hot-path 감소 | 난이도 | 구분 |
|---:|---|---:|---|---|
| 1 | C++ coordinator/node phase projection | send/request **-7** | 중간~높음 | 동작 보존 가능 |
| 2 | .NET Handoff ingress projection·capture claim | **-3~-4** | 높음 | 의미 리팩터링 |
| 3 | .NET Actor registry dispatch snapshot | **-2** | 중간 | generation fence 리팩터링 |
| 4 | C++ gateway + callback admission | **-2** | 높음 | owner/lifecycle 리팩터링 |
| 5 | Java Router recv | inbound당 -1 | 중간 | receive-owner assertion과 shutdown join 필요 |
| 6 | Java Subscriber subscribe | fanout inbound당 -1 | 중간~높음 | close reservation/join 필요 |
| 7 | .NET task runner/supervisor 중첩 lane | -1 | 높음 | `[의심-H]`, admission owner 통합 |
| 8 | .NET Spot claim/lane lookup | -1 | 중간 | 같은 turn projection 후보 |
| 9 | Java send/request/reply wrapper 5곳 | 해당 outbound당 -1 | 높음 | per-socket egress owner 필요 |
| 10 | C++ request bookkeeping | request -2 | 높음 | reservation transfer 의미 변경 |
| 11 | Java receive-flow wrapper 2곳 | pressure 전이당 -1 | 낮음 | `[의심]`, assertion·교차-operation test 선행 |
| 12 | .NET 공개 API rollback bridge 2곳 | 실패 호출당 -1 | 낮음~중간 | 동작 보존 회수 후보 |

C++의 5~6 목표는 1차 projection 통합만으로 달성되지 않습니다. 권장 단계는 `17/19 → 10/12`의 phase-preserving 변경을 먼저 분리하고, 이후 gateway/callback/request ownership과 unified admission token을 별도 의미 리팩터링으로 진행하는 것입니다.

## 검증 범위

- 변경 파일: **없음**
- branch 전환·commit·push 등 git mutation: **없음**
- build/test: 사용자 지시에 따라 **실행하지 않음**
- 조사 시작 시 현재 branch가 `main`임만 확인
- 주요 전수 검색:

```text
rg -o '\bAwaitStateLane\b' framework/languages/dotnet
rg -U -o 'GetAwaiter\s*\(\s*\)\s*\.\s*GetResult\s*\(' framework/languages/dotnet
rg -n --hidden --no-ignore '_barrierGate|barrierGate|BarrierGate' framework/languages/dotnet
rg -n --glob '*.java' '\bsynchronized\b' framework/languages/java/.../runtime/binding
rg --files framework/languages/cpp -g '*.cpp' -g '*.hpp' ...
```

C++ 최종 계수는 주석·문자열을 제외하고 `run(...)` 괄호를 닫은 뒤 이어지는 `.get()`을 판별하는 read-only lexical scan으로 확인했습니다.


