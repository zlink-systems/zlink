# R7 — Location·relocation·observability 심층 리뷰

감독자가 규칙의 소유 위치를 정리할 항목과 0.18.0 동작 변경 항목을 구분하기 위한 읽기 전용 진단이다. 지정된 한국어 spec 12개 파일, 5,669행을 모두 읽었다. 구현은 아래에 기록한 호출 경로를 정적으로 대조했다. Build, test, benchmark, conformance runner는 실행하지 않았다. 이 보고서 외의 파일은 변경하지 않았다.

`행동 변경: 있음`은 정상 입력뿐 아니라 장애·잘못된 입력·느린 observer에서 application이 관찰할 차이도 포함한다. 규칙 수는 각 finding에 명시한 정책 정의 또는 판정의 수다. 코드 행 수나 flag 개수를 규칙 수로 대신하지 않는다. Kotlin은 별도 `framework/languages/kotlin` 디렉터리가 아니라 Java core를 사용하는 `framework/languages/java/zlink-framework-kotlin`이다. 공유 근거는 `framework/languages/java/zlink-framework-kotlin/build.gradle.kts:14`이며, 별도 coroutine 경계의 실행 검증은 하지 않았다.

## 요약 표

| 번호 | 제목 | 분류 | 행동 변경 | 규칙 수 | 성능 영향 | 확신 |
|---|---|---|---|---|---|---|
| F-R7-1 | Owner unavailable 정책의 소유 문서 중복 | consolidation | 없음 | 3 → 1 | 없음 | 높음 |
| F-R7-2 | Observability에 재정의된 fanout ready 판정 | consolidation | 없음 | 3 → 1 | 없음 | 높음 |
| F-R7-3 | Node mesh가 shutdown admission seal을 조회하지 않음 | scattered-control | 있음 | 2 → 1 | 없음 | 높음 |
| F-R7-4 | .NET shutdown 게시 실패 뒤의 시간 대기·재제출 | spec-impl-drift | 있음 | 2 → 1 | 있음: 종료 중 timer·게시 제거 | 높음 |
| F-R7-5 | Relay-ready 이후 source 복원을 허용하는 상충 규칙 | scattered-control | 있음 | 2 → 1 | 있음: source 재판정 제거 | 높음 |
| F-R7-6 | StoreFailureGrace 안에서 새 연결을 만드는 공통 구현 | spec-impl-drift | 있음 | 2 → 1 | 있음: 장애 중 connect 제거 | 높음 |
| F-R7-7 | Host terminal 무유실 문장과 bounded observer 계약 | spec-impl-drift | 있음 | 2 → 1 | 없음 | 중간 |
| F-R7-8 | 성공한 immutable Put의 bytes를 다시 확인하는 상위 계층 | lower-layer-reverification | 있음 | 2 → 1 | 있음: blob별 Read·비교 제거 | 높음 |
| F-R7-9 | Diagnostics level에 종속된 protocol 오류 판정 | scattered-control | 있음 | 3 → 1 | 없음: 제안은 실패 의미 정리 | 높음 |
| F-R7-10 | Conformance adapter의 오래된 테스트 식별자 | gate-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R7-11 | 현재 Session 계약과 다른 conformance 기대 모델 | gate-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R7-12 | 공개 검증 절에 중복된 white-box 비용 조건 | form | 없음 | 6 → 3 | 없음 | 높음 |

행동 변경 없이 제거 가능하다고 확정한 `lower-layer-reverification`·`scattered-control` 항목은 없다. 따라서 README의 다음 우선순위인 `consolidation`을 먼저 배치했다. Gate 항목은 application runtime을 수정하지 않는 제안이며, gate 통과를 확인했다는 뜻은 아니다.

## Findings

### F-R7-1 Owner unavailable 정책의 소유 문서 중복

- 분류: consolidation
- 위치: `framework/doc/framework/common/spec/server/05-location-relocation/06-failure-failover-policy.ko.md:101`, `:109`, `:117`, `:129`, `:168`, `:194`, `:252`, `:290`; `framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:148`, `:304`, `:327`, `:329`, `:539`, `:554`; `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:33`, `:67`, `:81`. 조회 status의 투영은 `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md:875`에도 있다.
- 현재 규칙(인용): “이 문서가 공개 장애 동작의 소유 문서다.” / “Operation별 적용 표와 owner가 사라졌을 때의 결과가 그곳에 있다.” / “Current operation을 [`Unavailable`](../00-foundation/07-framework-error-model.ko.md)로 끝낸다.”
- 문제: Failure policy §9는 공개 장애 결과를 자신이 소유한다고 선언하면서 §4.1은 같은 결과의 정의를 routing에 돌려준다. Routing §2.6은 실제로 공개 operation의 결과와 자동 재제출 금지를 정의하고, error model은 같은 원인→kind 매핑을 다시 적는다. 구현 대조에서는 owner가 없는 경우와 authority는 있지만 owner를 사용할 수 없는 경우를 구분한다. 새로운 실패 정책이 필요한 문제가 아니라, R4·R6·R7이 같은 정책을 따로 바꿀 수 있는 문서 소유권 문제다. Error kind의 전체 정의와 resolver tag의 표현은 각각의 원래 계층에 남길 수 있다.
- 제안: `06-failure-failover-policy.ko.md` §4.2가 소유하고 다른 문서는 결과를 참조한다 — **“기존 Ready Actor·Spot의 authority가 남아 있으나 current owner를 사용할 수 없으면 현재 operation은 Unavailable로 끝나며, Framework는 그 operation을 재제출하거나 authority를 해제해 다른 node에서 자동 활성화하지 않는다.”**
- 규칙 수: before 3 → after 1 — failure policy·routing·error model의 owner 장애 정책 정의를 한 곳으로 합친다. Kind 정의, resolver tag, 조회 status는 서로 다른 투영이므로 삭제하지 않는다.
- 행동 변경: 없음 — 현재 owner 장애의 결과와 다음 새 호출의 조회 동작을 유지한다.
- 영향: framework(cpp, dotnet, java, kotlin, node); 대표 경로 `framework/languages/cpp/framework/src/runtime/locations/store_location_resolvers.hpp:356`. 문서 소유권 정리만 제안한다. R6는 routing §2.6, R4는 error kind 정의를 이 소유권과 대조해야 한다.
- 성능 영향: 없음 — resolver와 terminal mapper를 바꾸지 않는다.
- 근거 코드: C++ `framework/languages/cpp/framework/src/runtime/locations/store_location_resolvers.hpp:356`; .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkStoreLocationResolvers.cs:194`; Java/Kotlin `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkStoreLocationResolvers.java:355`; Node `framework/languages/node/packages/framework/src/runtime/locations/resolvers.ts:420`. .NET·Node는 여기서 내부 결과 tag까지 확인했고 모든 public surface의 mapper를 실행 검증하지는 않았다.
- 확신: 높음 — 상호 소유권 선언과 중복 결과 문장이 명시적이다. R4의 `Unavailable`을 remote queue 포화로 한정해 읽을 수 있는 `07-framework-error-model.ko.md:94`는 R4에서 별도로 판단해야 한다.

### F-R7-2 Observability에 재정의된 fanout ready 판정

- 분류: consolidation
- 위치: `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:235`, `:430`; `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md:524`; 소유 transport 계약 `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:57`, `:171`, `:177`, `:229`, `:231`, `:255`, `:285`, `:371`, `:383`.
- 현재 규칙(인용): “Disconnect를 확인하거나 15초 동안 record가 없으면 해당 publisher만 후보에서” / “첫 정상 application record나 beacon을 받는다.”
- 문제: Monitoring의 status 설명에 첫 record 수신, 15초 timeout, 후보 제외라는 transport 행동이 다시 정의돼 있다. 이는 단순한 status field 설명을 넘어 연결의 수명과 선택 결과를 결정한다. Location runtime에도 첫 record를 ready의 조건으로 재서술한다. 실제 구현은 수신/liveness 소유자가 ready를 바꾸고 monitoring은 그 결과를 읽는다.
- 제안: `02-channel-transport/05-transport-liveness.ko.md` §4가 소유하고 monitoring은 그 판정의 status 투영만 설명한다 — **“Fanout publisher의 ready 여부는 publisher별 첫 정상 application record 또는 beacon 수신으로 시작해 disconnect나 15초 무수신으로 끝나며, discovery와 monitoring은 이 transport 판정 결과를 사용한다.”**
- 규칙 수: before 3 → after 1 — transport·location·monitoring의 readiness 정책 정의를 통합한다. Transport 문서의 정상 흐름·상태 표·검증 예시는 같은 소유자의 설명이다.
- 행동 변경: 없음 — 첫 수신 조건, timeout 값, publisher별 장애 격리를 유지한다.
- 영향: framework(cpp, dotnet, java, kotlin, node); `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts:1036`의 기존 소유자를 문서에 반영한다. R5 transport 검토와 연결한다.
- 성능 영향: 없음 — 별도 monitoring liveness timer를 새로 만들거나 기존 timer를 변경하지 않는다.
- 근거 코드: C++ `framework/languages/cpp/framework/src/runtime/fanout/raw_fanout_owner.cpp:375`; .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkAutomaticFanoutSubscriberRuntime.cs:390`; Java/Kotlin `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkClassicFanoutLiveness.java:127`; Node `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts:1036`, `:1155`. 각 경로의 ready 갱신과 만료 처리를 읽었으며 실제 clock 경계는 실행하지 않았다.
- 확신: 높음 — 문서 중복과 구현 소유자가 대응한다.

### F-R7-3 Node mesh가 shutdown admission seal을 조회하지 않음

- 분류: scattered-control
- 위치: `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:763`, `:766`, `:768`, `:775`, `:789`; 최신 결정 `doc/plan/c016-worklog/decisions.ko.md:1310`(D-097), `:1343`(D-098, 특히 3번).
- 현재 규칙(인용): “같은 seal을 mesh node도 따른다: 새 peer admission을 시작하지도 수락하지도 않는다(Hello를 보내지 않고, inbound Hello에 Admit을 보내지 않는다).”
- 문제: Node host는 `options.admission.seal(meshName, RuntimeShutdown)`로 application gate를 닫는다. 그러나 raw mesh는 그 gate를 참조하지 않고 empty frame에 Hello를 보내며, 새 Hello를 `admitPeer`로 처리한 뒤 Admit을 보낸다. 주기적인 `announceExpectedPeers`도 gate를 조회하지 않는다. `service-topology-registry`가 호출하는 connection policy는 Object Client 간 연결 필요성만 판단하므로 descriptor의 Draining 게시가 이 누락을 보충하지 않는다. .NET·C++·Java/Kotlin은 host의 기존 seal을 mesh에 연결한다. Node의 application admission 판단과 mesh peer admission 판단이 서로 독립된 것이 원인이다.
- 제안: `05-host-relocation-flow.ko.md` §14.1을 단일 소유자로 유지한다 — **“Host가 설치한 shutdown admission seal을 application·relocation unit·mesh peer admission이 함께 조회하며, seal 이후에는 새 Hello와 Admit을 제출하지 않는다.”**
- 규칙 수: before 2 → after 1 — Node의 application seal 판정과 독립적인 raw peer admission 판정을 기존 seal 하나로 통합한다. 별도 mesh shutdown flag를 추가하는 제안이 아니다.
- 행동 변경: 있음 — shutdown drain 중 새 peer가 admit되거나 Hello를 받는 현재 Node 동작이 달라진다.
- 영향: framework(node); 원인 `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:690`. 아래 shutdown 표에 cpp·dotnet·java·kotlin의 공유 seal 근거를 별도로 정리했다.
- 성능 영향: 없음 — 기존 seal 조회가 필요하며 제거되는 hot-path 검증을 주장하지 않는다.
- 근거 코드: Node seal `framework/languages/node/packages/framework/src/runtime/host/route-mesh-runtime.ts:570`; Hello/Admit `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:702`, `:765`; 지속적인 announce `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:1536`; connection policy `framework/languages/node/packages/framework/src/runtime/foundation/route-mesh-connection-policy.ts:18`. 다른 언어는 아래 표의 file:line을 확인했으며 live shutdown 경쟁은 실행하지 않았다.
- 확신: 높음 — 호출 경로와 gate 부재를 확인했다. 단순히 flag가 여러 개라는 이유로 판정한 항목이 아니다.

### F-R7-4 .NET shutdown 게시 실패 뒤의 시간 대기·재제출

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:771`; `doc/plan/c016-worklog/decisions.ko.md:1343`의 D-098 4번.
- 현재 규칙(인용): “게시의 terminal (성공 또는 실패)만 기다리며, 게시 뒤 전파를 위한 시간 대기는 두지 않는다.”
- 문제: .NET의 `PublishDrainingMarkerAsync`는 실제 descriptor mutation이 false를 반환하거나 예외로 끝나도 terminal로 소비하지 않는다. Catch 뒤 `PollingInterval`만큼 기다리고 동일한 상위 게시 절차를 다시 시작한다. `PublishServingWeightAsync`도 false 뒤 같은 시간 대기를 둔다. 따라서 성공한 게시 뒤의 전파 sleep을 제거했더라도, 실패한 게시의 terminal 뒤에는 shutdown deadline을 소모하는 별도 정책이 남아 있다. 이 단계가 끝나야 accepted operation barrier에 도달한다. Java/Kotlin과 Node의 host 경로는 게시 future의 완료를 한 번 소비한다. C++의 실제 mesh descriptor 게시도 실패를 결과로 소비한다.
- 제안: `05-host-relocation-flow.ko.md` §14.2가 소유한다 — **“Shutdown은 Draining 게시의 첫 terminal 결과를 소비해 기존 종료 결과·정리 경로로 진행하며, 게시 성공을 기다리는 별도 시간 대기나 재제출을 만들지 않는다.”**
- 규칙 수: before 2 → after 1 — 게시 terminal을 소비하는 규칙과 성공할 때까지 주기적으로 재제출하는 규칙을 전자로 통합한다. Store 내부의 불확정 CAS 확인 계약은 별개다.
- 행동 변경: 있음 — Store 게시 실패 시 shutdown 소요 시간, accepted 작업에 남는 정리 시간과 일부 종료 결과가 달라질 수 있다.
- 영향: framework(dotnet); `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkDrainExecutor.cs:512`, `:569`. 성공 경로만의 D-098-4 검증으로는 이 분기를 확인할 수 없다.
- 성능 영향: 있음 — shutdown 실패 경로의 추가 timer와 반복 descriptor 게시가 사라진다. 일반 message hot path에는 영향이 없다.
- 근거 코드: .NET 반복 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkDrainExecutor.cs:512`, `:569`; 실제 false/성공 반환 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkAutoConnectReconciler.cs:247`; Java/Kotlin `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:2099`; Node `framework/languages/node/packages/framework/src/runtime/host/route-mesh-runtime.ts:577`. C++ 대조는 아래 shutdown 표에 기록했다.
- 확신: 높음 — false와 예외 뒤의 대기가 명시적이며 첫 publication terminal과 host의 추가 재시도를 구분할 수 있다.

### F-R7-5 Relay-ready 이후 source 복원을 허용하는 상충 규칙

- 분류: scattered-control
- 위치: 복원 허용은 `framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:566`, `:573`. 복원 금지는 같은 파일 `:149`, `:218`, `:229`, `:505`, `:564`, `:568`, `:619`, `:630`, `:740`; `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md:1079`, `:1172`, `:1318`; `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:220`, `:439`, `:649`, `:677`, `:738`, `:965`; `framework/doc/framework/common/spec/server/05-location-relocation/06-failure-failover-policy.ko.md:190`, `:300`; `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:513`.
- 현재 규칙(인용): “Store가 여전히 source를 owner로 보이면 local dispatch를 복원하고 backlog를 다시 적용한다.” / “실패해도 Location Store가 source를 가리키는 동안 source dispatch를 다시 열지 않는다.”
- 문제: 동일한 relay-ready accepted 이후 cutover submit 불확정 상황에 대해 §9 failure 표와 바로 뒤 설명이 서로 다른 정책을 정의한다. C++ source의 reconciliation sweep은 Store가 source를 가리키면 실제로 backlog를 local에 다시 적용한다. 반면 Node는 `readyReceived` 이후 복원하지 않고 target convergence 실패로 끝내며, Java canonical cutover는 submit 성공·실패 모두 source 사본 보관 단계로 넘어간다. Target은 별도로 Restore deadline까지 CAS를 계속할 수 있으므로, source에서 읽은 한 시점의 owner가 source라는 사실만으로 target의 향후 CAS 권한이 끝났다고 판단할 수 없다. 예외를 유지하면 source와 target이 복원 가능성을 따로 판정해야 한다.
- 제안: 감독자가 비가역 경계를 확정한 뒤 `04-relocation-flow.ko.md` §4.4가 소유한다 — **“Relay-ready reply가 accepted 상태가 된 뒤에는 cutover 제출 결과나 source가 읽은 owner snapshot과 관계없이 source dispatch를 다시 열지 않으며, target만 기존 Restore deadline 안에서 owner 전환을 확정한다.”**
- 규칙 수: before 2 → after 1 — 무조건 비가역 규칙과 source snapshot에 따른 복원 예외를 전자로 합치는 초안이다.
- 행동 변경: 있음 — C++의 불확정 cutover 뒤 source local 처리·backlog 재실행 결과가 달라진다. 문장 정리만으로 무변경 처리하면 안 된다.
- 영향: framework(cpp); 원인 `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:6288`. .NET·Java/Kotlin·Node 전체 Actor Join/host relocation 조합의 동등성까지 입증한 것은 아니다.
- 성능 영향: 있음 — 이 예외만을 위한 source Store 재조회·복원 판정·reconciliation deadline 분기를 줄일 수 있다. Target의 기존 CAS 확인과 Message Follow 수명은 유지한다.
- 근거 코드: C++ `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:6176`, `:6288`; .NET Spot 경로 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotRetireTransport.cs:297`; Java/Kotlin `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkCanonicalRelocationStateMachine.java:261`; Node `framework/languages/node/packages/framework/src/runtime/host/service-relocation-host-runtime.ts:2079`. .NET source의 모든 Actor rollback 경로는 미확인이다.
- 확신: 높음 — spec 충돌과 C++의 해당 분기는 확정했다. 어느 규칙을 채택할지는 BLOCKERS에서 감독자에게 남긴다.

### F-R7-6 StoreFailureGrace 안에서 새 연결을 만드는 공통 구현

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md:1196`; `framework/doc/framework/common/spec/server/05-location-relocation/06-failure-failover-policy.ko.md:230`.
- 현재 규칙(인용): “이미 설정된 transport connection의 연결 상태 판단은 계속하지만 새 outbound connection은 만들지 않는다.” / “Grace가 끝난 뒤에도 descriptor 전체를 같은 시점의 목록으로 다시 읽기 전에는 새 connection을 만들지 않는다.”
- 문제: C++·.NET·Java/Kotlin·Node 모두 Store 실패를 확인한 뒤 grace가 남아 있으면 `lastDesired` 가운데 active에 없는 target에 Framework connect를 제출한다. 이는 Core가 이미 설치된 connection intent를 유지하는 것과 다르다. 문서상 grace 전후의 새 연결 결과는 모두 금지이므로 grace 만료 자체에 고유한 observable outcome이 없지만, 구현은 grace 안에서만 연결을 허용하는 별도 정책을 갖는다. 따라서 값만 삭제하는 무변경 단순화로 볼 수 없다.
- 제안: `01-location-runtime.ko.md` §10이 소유한다 — **“Descriptor snapshot을 완전히 다시 읽을 때까지 Store 실패 상태의 Framework는 기존 connection의 liveness만 유지하고 새 outbound connect를 제출하지 않는다.”**
- 규칙 수: before 2 → after 1 — grace 안의 connect 허용과 grace 뒤의 connect 금지를 완전한 snapshot 확보 여부 하나로 통합한다. Public 옵션 제거 여부는 별도 API 판단이며 이 제안에 포함하지 않는다.
- 행동 변경: 있음 — Store 장애 중 미연결 target에 새 연결이 생길 수 있는 현재 동작이 사라진다.
- 영향: framework(cpp, dotnet, java, kotlin, node); `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkAutoConnectReconciler.java:187`와 대응 구현.
- 성능 영향: 있음 — Store 실패 처리에서 이전 desired 목록을 순회해 새 connect를 제출하는 작업과 grace 판정용 상태·시간 비교를 줄일 수 있다. 정상 message hot path의 개선은 주장하지 않는다.
- 근거 코드: C++ `framework/languages/cpp/framework/src/runtime/locations/location_auto_connect_host_service.hpp:596`; .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkAutoConnectReconciler.cs:733`; Java/Kotlin `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkAutoConnectReconciler.java:187`; Node `framework/languages/node/packages/framework/src/runtime/locations/auto-connect-reconciler.ts:367`.
- 확신: 높음 — 모든 공통 runtime에서 동일한 grace 분기를 확인했다.

### F-R7-7 Host terminal 무유실 문장과 bounded observer 계약

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:865`; `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:287`, `:293`, `:298`, `:314`, `:322`, `:325`.
- 현재 규칙(인용): “Terminal event는 observer overflow로 잃지 않는다.” / “상한을 넘기면 framework는 가장 오래된 terminal status부터 버린다.”
- 문제: Host 문장은 observer overflow에 대한 무유실을 보장하지만 공통 monitoring은 terminal FIFO를 제한하고 폐기를 계수한다. .NET host status는 실제로 공통 bounded queue를 사용하며 relocation/termination result를 terminal로 분류한다. C++·Java/Kotlin·Node 공통 queue도 오래된 terminal을 버린다. 중간 status가 terminal을 덮어쓰지 않는 보장과, terminal이 무한히 보존되는 보장은 다르다. Host §16의 event가 별도 structured log만을 뜻한다면 그 표면 구분이 필요하며, 현재 문장만으로는 host observer의 예외 보장으로 읽힌다.
- 제안: 감독자가 event/status 표면을 확인한 뒤 `01-runtime-monitoring.ko.md` §6의 보관 규칙이 소유한다 — **“Host relocation·shutdown terminal status는 중간 status로 덮어쓰지 않지만 observer의 terminal 보관 상한을 넘으면 가장 오래된 항목을 폐기하고 해당 observer의 terminal 유실 누계로 알린다.”**
- 규칙 수: before 2 → after 1 — host 무유실 예외와 공통 bounded terminal 규칙을 후자로 통합하는 초안이다.
- 행동 변경: 있음 — 기존 구현과 같더라도 host observer의 공개 무유실 보장을 약화하는 해석이므로 무변경 항목으로 처리하지 않는다.
- 영향: framework(cpp, dotnet, java, kotlin, node); .NET host 연결 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkMaintenanceRuntime.cs:144`, `:700`.
- 성능 영향: 없음 — 기존 bounded queue를 유지하는 제안이다.
- 근거 코드: .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkObservationQueue.cs:77`; C++ `framework/languages/cpp/framework/src/runtime/diagnostics/runtime_observation.hpp:151`; Java/Kotlin `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/monitoring/ZLinkStatusPublisher.java:428`; Node `framework/languages/node/packages/framework/src/runtime/diagnostics/runtime-observation-queue.ts:212`. .NET 외 언어의 host event와 status stream의 모든 연결은 미확인이다.
- 확신: 중간 — bounded terminal 폐기는 확정했고 host 문장의 표면 해석은 감독자 확인이 필요하다.

### F-R7-8 성공한 immutable Put의 bytes를 다시 확인하는 상위 계층

- 분류: lower-layer-reverification
- 위치: Provider 보장 `framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md:57`, `:131`, `:160`, `:166`, `:171`, `:177`, `:290`; 상위 재확인 요구 같은 파일 `:142`, `:149`; `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md:1216`, `:1313`. 불확정 쓰기 재확인은 같은 location runtime `:1206`, `:1211` 및 relocation store `:299`의 별도 조건이다.
- 현재 규칙(인용): “같은 reference에 같은 bytes가 이미 저장되어 있다.” / “저장할 때는 모든 data chunk를 다시 읽어 bytes와 checksum을 확인한 뒤에만 맨 앞 목록을 저장한다.”
- 문제: Immutable provider가 `Stored` 또는 `AlreadyStored`로 보장한 bytes를 상위 tree writer가 다시 Read하고 비교한다. .NET `PutVerifiedAsync`는 성공 후 전체 bytes를 읽고 `SequenceEqual`을 수행하며, Node canonical tree도 같은 작업을 한다. 불확정 Put의 결과를 복원하는 Read와 달리 이 성공 경로 Read는 provider가 이미 확정한 보존 사실을 다시 증명한다. Provider는 opaque bytes를 소유하므로 manifest의 관계·순서·전체 checksum 검증까지 불필요하다는 뜻은 아니다. Renew 때 immutable bytes를 재검증하라는 문장도 같은 소유권 문제이며, 실제 renew 호출 경로 전체는 확인하지 않았다.
- 제안: `03-relocation-store-redis.ko.md` §4.1이 bytes 보존의 소유자가 된다 — **“유효한 보관 기간을 반환한 Stored·AlreadyStored를 해당 reference의 immutable bytes 게시 근거로 사용하고, 쓰기의 성공 여부를 받지 못한 경우에만 같은 reference를 다시 읽어 결과를 확인한다.”**
- 규칙 수: before 2 → after 1 — provider의 immutable bytes 보장과 상위 계층의 성공 후 보존 재검증을 전자로 통합한다.
- 행동 변경: 있음 — 성공한 Put 뒤 Read만 실패하는 장애에서 기존에 실패하던 activation·완료 기록 게시가 진행될 수 있다. Compliant provider를 전제해도 이 오류 결과 차이는 관찰 가능하다.
- 영향: framework(dotnet, node), cpp의 `AlreadyStored` 처리도 재검증 대상; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkRelocationTreeStore.cs:424`. Handoff payload는 source memory에서 직접 전달되므로 일반 Actor·Spot handoff 전체에 blob 왕복 절감을 적용해 계산하면 안 된다.
- 성능 영향: 있음 — 확인한 tree writer의 blob별 추가 Read, payload 수신·비교가 사라진다. Cold activation 및 pending request 완료 기록의 저장 경로에 해당한다.
- 근거 코드: .NET tree `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkRelocationTreeStore.cs:430`; Node tree `framework/languages/node/packages/framework/src/runtime/actors/deferred-join-accepted-journal.ts:993`; C++ repository `framework/languages/cpp/framework/src/runtime/locations/provider_relocation_repository.hpp:64`; Java/Kotlin repository `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/locations/ZLinkProviderRelocationRepository.java:79`; 불확정 응답과 구분하는 .NET repository `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkProviderRelocationRepository.cs:85`. Java/Kotlin repository의 정상 결과에는 Read가 없으며 상위 tree 전체는 미확인이다. C++는 Stored에서 바로 반환하지만 AlreadyStored는 Read로 내려간다.
- 확신: 높음 — 성공 후 Read와 provider 보장을 직접 대조했다. Framework가 소유한 payload 의미 검증이나 Store 결과 유실 확인을 삭제하는 제안이 아니다.

### F-R7-9 Diagnostics level에 종속된 protocol 오류 판정

- 분류: scattered-control
- 위치: `framework/doc/framework/common/spec/server/06-observability/04-flow-correlation.ko.md:65`, `:68`, `:73`, `:78`, `:89`, `:94`, `:104`; `framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md:275`.
- 현재 규칙(인용): “flow 정보는 protocol error다.” / “각 처리 지점이 `Off`를 확인한 뒤에는 flow ID 생성, validation, context capture” / “Tracing은 routing, handler 분배와 lifecycle 결정을 바꾸지 않는다.”
- 문제: 잘못된 flow pair를 operation 실패 또는 connection 종료로 만드는 규칙과 Off에서 그 검증을 생략하는 규칙이 동시에 존재한다. 확인한 네 언어의 envelope decoder는 실제로 flow capture flag에 따라 같은 잘못된 flow 입력을 거부하거나 무시한다. 따라서 diagnostics level을 바꾸면 handler에 도달하는 message가 달라질 수 있다. 이 행동 규칙이 observability 안에만 있으며 protocol 판정과 관측 비용 판정이 같은 분기를 소유한다. `correlation_id`는 필수 protocol 정보이므로 optional flow pair와 분리해야 한다.
- 제안: BLOCKERS의 선택 후 `02-channel-transport/06-wire-protocol.ko.md`의 envelope 검증 절이 소유한다 — **“Protocol 경계는 correlation_id를 항상 검증하고, 관측 전용 flow pair의 오류는 flow context를 만들지 않는 것으로만 처리하여 diagnostics level이 message 완료·dispatch·connection 수명을 바꾸지 않게 한다.”**
- 규칙 수: before 3 → after 1 — malformed flow의 protocol 실패, Off 검증 생략, tracing 비간섭이라는 충돌하는 판정을 protocol 경계의 비간섭 규칙으로 통합하는 초안이다.
- 행동 변경: 있음 — tracing이 켜진 runtime에서 잘못된 flow field 때문에 실패하던 message가 처리될 수 있다. 반대로 항상 엄격히 검증하는 대안을 택하면 Off runtime의 결과가 바뀐다.
- 영향: framework(cpp, dotnet, java, kotlin, node); `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:509`. R5의 wire/STREAM 소유권 검토가 필요하다.
- 성능 영향: 없음 — 이 finding은 오류 결과의 소유권을 다룬다. UUID 검증·capture 비용의 절감 수치는 제안하지 않는다.
- 근거 코드: C++ `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:177`; .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:509`; Java/Kotlin `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:225`; Node `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts:558`. STREAM decoder 전체는 미확인이다.
- 확신: 높음 — envelope 수준의 조건부 거부는 코드로 확정했다. 통합 문장의 선택은 감독자에게 남긴다.

### F-R7-10 Conformance adapter의 오래된 테스트 식별자

- 분류: gate-drift
- 위치: 관련 계약 `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:435`, `:455`, `:463`, `:508`; 연결 위치 `framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:525`; baseline 기록 `doc/plan/c016-worklog/decisions.ko.md:1377`. Fixture의 식별자 계약 `framework/runtime/conformance/relocation-conformance-adapters-v1.json:21`, `:102`, `:254`, `:264`.
- 현재 규칙(인용): `"Canonical_Seal_Retries_Target_Push_Until_Command_44_Commits"` / `"OnJoinedActor public bound-session push waits for route convergence and is delivered exactly once"`.
- 문제: Runner는 `--list`를 처리하기 전에 validator를 import한다. Validator는 adapter의 문자열이 실제 source에 존재해야 한다고 검사한다. 24개 evidence identifier를 읽기 전용 문자열 검색으로 대조한 결과 위 .NET·Node 2개가 없었다. .NET의 첫 실패를 고쳐도 Node의 두 번째 누락이 남는다. 현재 .NET 테스트는 `Canonical_Seal_Routes_Command_44_Without_Store_Reread`, Node 테스트는 `relocation target binding republish delivers the post-Join bound-session push exactly once`다. Git 이력상 각각 `36ded9f8fd`, `7a6e6ce751`에서 교체됐으며 단순 삭제가 아니다. Node focused command의 정규식도 이전 이름을 사용해 해당 시나리오 선택이 빠진다. 현재 테스트가 새 계약 방향으로 바뀐 뒤 adapter와 실행 선택자가 남은 drift다.
- 제안: `relocation-conformance-adapters-v1.json`의 evidence 등록이 소유한다 — **“현재 계약을 검증하는 실제 테스트의 식별자를 evidence와 focused 실행 선택자가 함께 사용한다.”**
- 규칙 수: before 2 → after 1 — 실제 테스트와 별도 과거 이름에 의존하는 gate 선택 규칙을 현재 식별자 하나로 통합한다. Validator 우회나 `--list` 검증 생략은 제안하지 않는다.
- 행동 변경: 없음 — application runtime은 그대로이며 gate가 현재 테스트를 찾고 선택하게 한다.
- 영향: framework conformance(dotnet, node); `framework/runtime/conformance/relocation-conformance-adapters-v1.json:102`, `:264`. C++·Java·Kotlin을 포함한 나머지 evidence 문자열의 존재는 확인했으나 테스트 실행·coverage는 확인하지 않았다.
- 성능 영향: 없음 — application 경로와 무관하다.
- 근거 코드: import 순서 `scripts/run-framework-relocation-conformance.mjs:24`, `:100`; 문자열 assertion `framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs:3274`; .NET 현재 테스트 `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/SessionActorCoordinatorTests.cs:1320`; Node 현재 테스트 `framework/languages/node/test/contract/stream-runtime.test.js:4160`.
- 확신: 높음 — `--list` 자체는 실행하지 않았지만 사용자가 준 baseline 실패의 선행 조건과 후속 누락을 정적으로 확인했다. 이름 수정만으로 fixture 전체가 현 계약에 맞는 것은 아니며 F-R7-11이 남는다.

### F-R7-11 현재 Session 계약과 다른 conformance 기대 모델

- 분류: gate-drift
- 위치: `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:435`, `:455`, `:463`, `:500`, `:508`, `:524`; `framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:525`; `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:579`, `:683`, `:929`. 상충 fixture `framework/runtime/conformance/bound-session-relocation-v1.json:117`, `:131`, `:149`, `:155`.
- 현재 규칙(인용): “Session owner는 이 두 검증을 반복하거나 서로의 결과를 다시 판단하지 않는다.” / `"remoteMissingProofStoreReadCount": 1` / `"retryUntil": "exactRouteTerminalOrShutdown"`.
- 문제: 현재 Session spec은 Session identity·binding·relocation identity의 확인만 Session owner에 두며 Store 읽기와 target 준비 재판정을 금지한다. 그런데 fixture와 validator는 remote target proof Store Read 1회, immutable target proof acceptance, exact route terminal에 따른 successor gate를 여전히 요구한다. 이는 오래된 문자열만의 문제가 아니라 golden 기대 모델이 별도 정책 소유자로 남은 것이다. .NET의 교체 테스트는 Store Read를 차단한 상태에서 command 44가 성공하고, push-before-route와 route-before-push 모두 한 번 전달되는지를 확인한다. Node의 확인한 target proof helper는 binding 소유 값만 대조하며 Store proof 획득을 요구하는 fixture보다 좁다. 모든 언어가 새 계약을 완전히 구현했다고 판정할 근거는 아니다.
- 제안: `04-session/02-session-actor-binding.ko.md` §8.1이 소유하고 fixture는 그 관찰 결과를 투영한다 — **“Conformance는 Session owner의 현재 binding 검증, command 44의 원자적인 route·held message 전환과 seal timeout 결과를 검증하며 Store proof 재획득이나 route 완료 재시도를 독립적인 성공 조건으로 정의하지 않는다.”**
- 규칙 수: before 2 → after 1 — 공통 Session 계약과 fixture 내부의 별도 relocation 성공 모델을 공통 계약 하나로 합친다.
- 행동 변경: 없음 — 제안 범위는 fixture·validator의 계약 적응이며 runtime을 바꾸지 않는다. 이 정리로 드러나는 언어별 runtime 차이는 별도 행동 변경 진단이 필요하다.
- 영향: framework conformance(all languages); `framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs:1450`, `:1486`. R5가 Session 계약의 소유자이며 R7은 relocation gate와의 연결을 보고한다.
- 성능 영향: 없음 — runtime의 Store Read가 이 보고서로 제거되는 것은 아니다.
- 근거 코드: 과거 proof assertion `framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs:1450`; 과거 Store Read 수 assertion 같은 파일 `:1486`; 현재 .NET 관찰 `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/SessionActorCoordinatorTests.cs:1438`, `:1456`; Node 검증 범위 `framework/languages/node/packages/framework/src/runtime/streams/actor-session-binding-registry.ts:1583`. C++·Java·Kotlin의 Session route 구현 전체는 이 finding에서 미확인이다.
- 확신: 높음 — fixture·validator와 현재 spec의 규칙 불일치는 명시적이다. 현재 테스트의 assertion을 과거 fixture에 맞추는 방향은 계약 서술 원칙에 맞지 않는다.

### F-R7-12 공개 검증 절에 중복된 white-box 비용 조건

- 분류: form
- 위치: `framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:324`, `:327`, `:337`, `:343`; `framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md:263`, `:268`, `:307`, `:333`, `:336`, `:338`; 기준 `doc/principal/documentation/spec-writing-guide.ko.md:367`, `:378`, `:403`, `:701`.
- 현재 규칙(인용): “공개 표면” / “수집을 위해 전체 object나 Store record를 순회하지 않는다.” / “호출부 코드로 확인한다”.
- 문제: 검증 절은 공개 표면만으로 확인한다고 선언하지만 내부 순회 금지, allocation·queue 생성, 호출부 lambda 생성 여부를 요구한다. 이 조건은 필요할 수 있으나 guide §4.4가 규칙 문단의 내부 확인 조건으로 분류하는 항목이다. 특히 tracing §5의 언어별 재량에는 관찰 결과와 재량 이유가 이미 적혀 있으므로 같은 호출부 코드 검사를 §7에서 다시 정의할 필요가 없다. 비용 계약을 삭제하거나 느슨하게 만드는 문제가 아니다.
- 제안: 각 비용 규칙 문단이 내부 확인 조건을 소유하도록 검증 절에서 중복 지시를 삭제한다 — **“공개 검증 절은 설정·계기·trace·operation 결과의 관찰만 서술하고, 순회·할당·호출부 코드의 내부 확인 조건은 해당 비용 규칙 문단에서 한 번 정의한다.”**
- 규칙 수: before 6 → after 3 — 전체 순회 금지, 관측 객체·복사 생성 금지, gate 앞 문자열·lambda 생성 금지의 본문 3조건과 white-box 검증 지시 3개를 본문 3조건으로 모은다.
- 행동 변경: 없음 — 비용 요구와 공개 결과를 그대로 유지하고 확인 조건의 위치를 정리한다.
- 영향: framework(all languages) spec; 기존 gate 예 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkMessageFlowTracer.cs:45`.
- 성능 영향: 없음 — gate·allocation 동작을 수정하지 않는다.
- 근거 코드: .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkMessageFlowTracer.cs:45`; C++ `framework/languages/cpp/framework/src/runtime/diagnostics/flow_context.hpp:84`; Java/Kotlin `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/diagnostics/ZLinkMessageFlowTracer.java:74`. Node의 trace 객체 생성 경로와 언어별 allocation 계측은 미확인이다.
- 확신: 높음 — 공개 interface 관찰과 white-box 검사를 분리하라는 guide에 직접 해당한다.

## 상태·deadline별 관찰 결과 대조

다음 표의 `L`, `LS`, `RS`, `R`, `H`, `F`는 각각 `framework/doc/framework/common/spec/server/05-location-relocation/` 아래 `01-location-runtime.ko.md`, `02-location-store-redis.ko.md`, `03-relocation-store-redis.ko.md`, `04-relocation-flow.ko.md`, `05-host-relocation-flow.ko.md`, `06-failure-failover-policy.ko.md`의 약칭이다. 표에만 약칭을 사용한다. 상태 이름이 다르다는 사실만으로 유지 필요성을 인정하지 않고, callback·수신·결과·남은 책임의 차이를 적었다. 문서가 이름만 제시한 경우에는 별도 저장 상태의 필요성을 확인하지 못했다고 표시했다.

| 상태·경계 | Spec 근거 | 다른 경계와 구분되는 관찰 결과 | 단순화 판단 |
|---|---|---|---|
| `SourceRunning` | R:624 | Source에서 application turn이 진행된다. | `SourcePaused`와 결과가 다르다. |
| In-flight payload 예산 대기 | H:412, H:917 | 다음 unit은 seal을 설치하기 전에 기다리므로 그 unit의 기존 application 처리가 계속된다. | Seal 이후 hold와 관찰 결과가 다르다. 별도 timeout이나 새 실패 kind로 만들지 않는다. |
| Unit seal / `SourcePaused` / S0 | R:149, R:155, R:625; H:468 | 해당 unit의 새 dispatch는 멈추고 도착한 message는 보관된다. 다른 unit까지 모두 멈추는 host seal과 범위가 다르다. | Unit과 host seal을 같은 boolean으로 합칠 근거가 없다. |
| `Preparing` | L:1005, L:1055 | Source owner를 유지하고 Capture 전이다. Source 종료 시 이동 취소 대상이다. | 일반 message caller는 pause와 같은 결과를 볼 수 있다. 별도 durable phase의 필요성은 callback·복구 경계를 함께 확인해야 한다. |
| `Captured` | L:1005, L:1056, L:1079 | Capture callback이 완료돼 memory payload로 명시적 실패를 복원할 수 있다. | Ownership 결과만 보면 Preparing과 같다. Capture 완료와 복원 원본의 수명은 달라 무변경 병합으로 확정하지 않았다. |
| Source ingress hold | R:155, R:447 | Capture 뒤 도착한 send는 수락·보관으로 끝나고 request는 기존 reply/deadline을 계속 기다린다. | Saved work와 겹쳐 보내면 중복 실행된다. 별도 보관 범위는 필요하다. |
| `TargetRestoring` / temporary queue | R:160, R:186, R:625 | Target Restore callback이 실행되지만 application message dispatch는 아직 열리지 않는다. | 생성·Restore 실패라는 callback 결과가 있어 RelayReady와 구분된다. |
| `Prepared` / `RelayReady` accepted | L:1006, L:1057; R:218, R:626 | 준비 완료 reply가 수락돼 source 복원을 금지하는 경계를 지난다. | Prepared 기록과 reply acceptance가 같은 사건인지 별도 상태인지 전 언어에서 입증하지 않았다. 같은 이름으로 가정하지 않는다. |
| `CutoverReceived` | R:241, R:627 | Boundary batch의 완전성을 확인하고 saved work→boundary relay→나머지 temporary work 순으로 진행할 수 있다. | Fallback과 순서 보장이 다르다. |
| `CutoverFallback` | R:249, R:561, R:627 | `cutover_timeout` Warning 후 완전성 확인 없이 CAS를 진행하며 late relay와 새 target message의 상대 순서를 보장하지 않는다. | 단순한 성공 동의어가 아니다. |
| `StoreRetry` | R:278, R:506, R:628 | Dispatch를 계속 닫고 같은 Restore deadline 안에서 owner 확인을 기다린다. 새 public error kind는 없다. | `CAS 대기`의 내부 진행 원인으로 설명 가능하다. 독립 enum·flag 강제는 필요성이 입증되지 않았다. |
| `OwnerCommitted` / S2 | L:1007, L:1058; R:628; H:470 | Location Store의 owner가 target으로 바뀐다. Source로 돌아갈 수 없지만 lifecycle·queue 전환이 남을 수 있다. | TargetOpen과 아직 처리 가능한 message가 달라 구분한다. |
| `TargetOpen` / S3 / `Ready` | L:1102, L:1115; R:629; H:471 | Queue 병합·regular route 전환·lifecycle 완료 뒤 target dispatch가 열린다. | Owner CAS만으로 이 결과를 대신할 수 없다. |
| `TargetRemoved` | L:1191; R:284, R:629 | Owner 확인 실패로 준비된 object·queue를 제거하고 Session update를 보내지 않는다. | 성공 후 TargetOpen으로 이어지는 상태가 아니다. R:628–630의 일렬 화살표는 성공/실패 분기로 읽어야 한다. |
| `Completed` | L:1059, L:1111, L:1141 | Target Ready 자체를 막지 않는 후속 기록이며 accepted request 완료·전달 대기 책임과 연결된다. | L:1059는 dispatch/update 완료를, L:1141은 accepted=completed 및 pending=0을 요구한다. 충분조건인지 필요조건 요약인지 감독자 확인 전 상태 삭제를 제안하지 않는다. |
| Reply `TerminalReceived` / `AlreadyTerminal` | L:1144 | Source가 첫 결과를 받았거나 이미 terminal임을 응답해 해당 결과의 보관 책임을 끝낸다. | 원래 caller의 최종 결과는 같지만 첫 전달/중복 확인이 다르다. 새로운 public 상태로 노출할 필요는 없다. |
| Reply `SourceLeaseExpired` | L:1148 | 수신 확인 없이도 종료된 source의 보관 책임을 끝낸다. | Live source의 ACK와 증거 소유자가 달라 내부 구분을 유지한다. |
| `FollowOnly` | R:588, R:630 | Source는 이전 주소에 온 message를 target으로 전달하고 handler를 실행하지 않는다. | SourceRunning과 명백히 다르다. |
| S1 — cutover submit terminal | H:469, H:713, H:721; R:218 | 성공·실패 모두 source의 cutover 제출 시도가 끝난다. Host Relocated 관찰과 사본 보관 창의 시작점이다. | Target CAS 확인 S2와 동의어가 아니다. |
| S4 — Message Follow route 제거 가능 | H:472, H:804 | 이전 route를 계속 사용할 책임이 끝나며 source의 SafeToShutdown 판정에 참여한다. | TargetOpen이나 host Relocated와 다르다. |
| `SafeToShutdown` | H:804 | 모든 source-local S4와 재전송 창 종료를 확인한 관찰 값이다. 이전에 Shutdown을 호출하는 것도 허용된다. | 새로운 admission/ACK gate가 아니라 기존 책임 종료의 파생 status로 둔다. |
| Host `Preparing` / `Serving` | H:192 | Startup 진행·신규 작업 거부와 정상 serving이 구분된다. | Public status·admission 차이가 있다. |
| Host `Relocating` | H:192, H:220, H:835 | 새 workload admission을 제한하고 현재 unit을 옮긴다. Shutdown의 신규 peer seal과 같지 않다. | `Draining`과 합치면 relocation용 control/admission이 달라진다. |
| Host `Relocated` | H:192, H:683, H:797 | Source workload는 이전됐으나 Message Follow·listener·peer infrastructure가 남을 수 있다. | Stopped 또는 SafeToShutdown과 다르다. |
| Host shutdown seal / `Draining` | H:763, H:766 | Host 전체 신규 작업·새 unit·새 peer를 막고 이미 수락한 책임을 정리한다. | Seal 판정의 소유자는 하나여야 한다. F-R7-3. |
| Host `Stopped` / `Error` | H:192, H:242, H:780 | 정상 종료와 runtime 오류는 public status·종료 reason으로 구분된다. | 결과의 차이를 지우는 병합은 불가하다. |
| Session binding seal | R:525; `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:453` | 같은 physical Session을 유지하며 해당 binding의 message를 held queue에 보관한다. | Host/unit seal과 범위·해제 사건이 다르다. |
| `Reserved` / `Active` / `Missing` | L:613 | 생성 전 수용 공간 확보, 실행 가능한 authority, object 부재가 구분된다. | CAS의 허용 전환을 결정하므로 같은 상태로 합치지 않는다. |
| `Creating` / 최초 message 복원 전 `Ready` | L:800; F:166 | 최초 activation 복구는 같은 incarnation으로 계속하되 아직 새 message를 실행하지 않는다. | 일반 Ready owner 장애의 자동 failover와 다르다. |

| Deadline·시간·cursor | Spec 근거 | 만료·경과의 고유한 결과 | 다른 시한과의 관계 |
|---|---|---|---|
| Owner lease TTL 15초 | L:577, L:584 | Store clock에서 lease 유효성이 끝난다. | Local fencing margin을 포함한 admission 시한과 같은 시각이 아니다. |
| Owner local admission deadline / fencing margin 5초 | L:579, L:584, L:589 | 새 message·timer 시작, factory/restore 확정, relocation 변경·수용 공간 확보를 막는다. | Host 전체의 lease 갱신 하나가 소유한다. Object deadline은 연장할 수 없다. |
| Lease renew interval 5초 / renew timeout 3초 | L:570–579 | 전자는 갱신 시작 기회, 후자는 갱신 operation의 응답 기한이다. | 그 자체가 새 public owner 상태가 아니다. 최종 admission 판정은 위 deadline에 모인다. |
| Host Relocate deadline 기본 30초 | H:98, H:348, H:496 | 새 unit 시작을 중단한다. Relay-ready 이후 unit은 target의 Restore 기한으로 계속한다. | 뒤의 동일 호출은 새 deadline을 만들지 않는다. |
| Target Restore absolute deadline | L:1185; R:279 | Target owner를 확인하지 못하면 target object·queue를 제거하고 update를 보내지 않는다. | 별도 CAS retry timeout을 만들지 않는다. Host waiter deadline과 소유자가 다르다. |
| `RelocationCutoverWaitTimeout` 기본 1,000ms | R:241, R:249 | Target이 cutover를 받지 못해도 Warning 후 fallback CAS를 진행한다. | Relay-ready reply 송신부터 계산한다. |
| Source cutover 재전송 창 | R:222; H:713 | 최초 cutover submit terminal 이후 source가 보유한 batch·cutover 사본의 재전송 책임이 끝난다. | Cutover timeout과 값은 같지만 시작 사건과 소유자가 달라 하나의 절대 deadline으로 합치지 않는다. |
| Message Follow duration 기본 30초, 0이면 사용 안 함 | F:124; L:832; H:797 | 이전 route를 통한 전달 가능 기간이 끝난다. | Session route update 지연이 연장하지 않는다. Target dispatch 중단 시간이 아니다. |
| Source reconciliation deadline | R:566 | C++는 Store 결과에 따라 target 채택/local 복원/Unavailable을 결정한다. | Message Follow duration을 재사용하는 별도 판정이다. F-R7-5의 상충 규칙에만 필요하다. |
| `RouteCacheMaxAge` 기본 15초 | L:832 | 다음 새 operation은 owner를 다시 조회한다. | Owner admission 시한을 넘지 못하며 Message Follow 기간과 역할이 다르다. |
| Session seal timeout 기본 3,000ms | R:525, R:563; `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:500` | Physical Session을 닫고 binding·held·seal을 정리한다. | Target owner CAS를 취소하거나 source를 복원하는 시한이 아니다. |
| Shutdown deadline 기본 30초 | H:780, H:904 | Bounded teardown 후 ForceStopped/DeadlineExceeded 또는 TeardownFailed로 끝난다. | Publication 뒤의 추가 전파 대기를 허용하지 않는다. F-R7-4. |
| Draining publication terminal | H:771 | 성공·실패를 소비하고 종료 절차가 진행할 수 있다. | 시간 구간이 아니라 완료 사건이다. 별도 grace가 필요하지 않다. |
| Source 중단 1초 관측 목표 | H:486 | 경고·구간 관측만 달라지고 실패·rollback 조건은 바뀌지 않는다. | 숫자가 같아도 1,000ms cutover fallback과 합치면 행동 변경이다. |
| `StoreFailureGrace` | L:1196; F:230 | Spec에서는 전후 모두 새 connect 금지여서 별도 만료 결과가 없다. | 구현은 전후가 다르다. F-R7-6. |
| Blob retention 기본 24시간 / renew threshold 12시간 | RS:152 | 전자는 payload가 만료돼 Missing/DataLost가 될 수 있는 시점, 후자는 보관 연장 시작 기준이다. | Restore retry 기한으로 사용하지 않는다. |
| Creation terminal retention: 최초 deadline + 5분 | L:735 | 같은 생성 operation 결과를 다시 확인할 수 있는 보관 범위가 끝난다. | Object/owner 수명과 다르다. |
| Snapshot cursor `Expired` | LS:133, LS:138, LS:140 | 이전 page 결과를 버리고 같은 조회를 첫 page부터 다시 시작한다. | Cursor는 snapshot identity다. Owner fence나 새로운 object 상태가 아니다. |
| Message 자체의 request deadline·cancellation | R:158, R:447, R:599 | Original caller의 request가 terminal로 끝난다. | Hold/relay가 새 application request를 만들거나 caller의 기한을 연장하지 않는다. Hop-local relay 대기와 구분한다. |

`StoreRetry`, Preparing/Captured의 별도 저장 표현, `Completed`의 기록 시점은 complexity 검토 후보지만 **observable 차이가 없다는 증거 없이 삭제를 확정하지 않았다**. 확정된 중복 판정은 F-R7-5·F-R7-6에 반영했다. S0–S4는 상태 flag 목록이 아니라 서로 다른 소유자의 사건 시각이며, node 간 clock을 빼서 하나의 deadline으로 만드는 대안은 계약과 다르다.

## Shutdown seal의 언어별 근거

공통 기준은 `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:766` 및 `:771`이다. 아래는 각 runtime의 생산 코드 경로를 읽은 결과이며 경쟁 시나리오 실행 결과가 아니다.

| 언어 | Seal의 기존 소유자와 host·relocation 연결 | Mesh의 조회 | 게시 뒤 대기 판정 |
|---|---|---|---|
| C++ | `framework/languages/cpp/framework/src/runtime/host/app.cpp:3545`가 shared atomic을 변경하고 `:1375`가 같은 포인터를 Spot에 전달한다. `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:10341`, `:11495`가 신규 join/create에 사용한다. | `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:800`이 같은 `drain_flag`를 `shutdown_admission_seal`에 연결한다. `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:2764`, `:3910`이 inbound/outbound Hello에서 조회한다. | `framework/languages/cpp/framework/src/runtime/host/app.cpp:3677`은 전파 대기 인자 false만 전달하고 `:3679`에서 실제 mesh 게시 결과를 소비한다. `:3645` 뒤 timed wait는 이 호출에서 실행되지 않는다. |
| .NET | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkDrainAdmissionGate.cs:99`의 ClaimShutdown과 `:118`의 IsSealedForShutdown이 소유한다. `:138`의 relocation fence도 같은 gate 소유자 안에 있다. `_draining`, `_sealed`, `_owner`는 owner 종류와 단계가 달라 단순 중복 flag로 세지 않았다. | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotNodeInitializer.cs:27`의 delegate를 `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8078`, `:8482`가 조회한다. `:8377`은 Draining을 topology 사건으로 되돌리지 않는다. | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkDrainExecutor.cs:123` 이후 성공 경로는 accepted barrier로 간다. 다만 `:512`, `:569`의 실패 뒤 timer·재제출은 F-R7-4다. |
| Java | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/drain/ZLinkMeshDrainCoordinator.java:24`, `:37`, `:66`의 per-mesh state가 claim과 seal 조회를 함께 소유한다. `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:2066`이 sealAll을 호출한다. `drainStarted`는 host 종료 operation의 중복 시작을 막는 별도 lifecycle 값이다. | 같은 runtime `:316`이 coordinator 조회를 mesh에 연결한다. `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:6350`, `:6919`가 Hello 수락·송신을 막는다. | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:2099`에서 publication 완료를 한 번 받고 `:2108`에서 barrier로 간다. |
| Kotlin | `framework/languages/java/zlink-framework-kotlin/build.gradle.kts:14`가 Java core API를 공유한다. `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:1`의 coroutine facade를 확인했다. | 공용 Java runtime의 위 seal 연결을 사용한다. 독립 Kotlin mesh/shutdown 구현을 찾지 못했다. | Java와 같은 runtime 경로다. Kotlin coroutine cancellation까지 별도로 실행 검증하지 않았다. |
| Node | `framework/languages/node/packages/framework/src/runtime/admission.ts:33`의 per-mesh gate를 `framework/languages/node/packages/framework/src/runtime/host/route-mesh-runtime.ts:571`이 닫는다. | `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:363`, `:702`, `:765`는 이 seal을 조회하지 않는다. `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:1536`의 announce도 계속 가능한 구조다. F-R7-3. | `framework/languages/node/packages/framework/src/runtime/host/route-mesh-runtime.ts:577`의 게시 완료 뒤 `:580`에서 accepted barrier로 간다. 별도 전파 대기는 없다. |

## 추가 후보와 제외한 해석

- Snapshot scan의 직접 Read 재요구(`framework/doc/framework/common/spec/server/05-location-relocation/02-location-store-redis.ko.md:140`)는 snapshot의 bytes/version 및 CAS 보장과 겹칠 수 있으나, 실제 recovery mutation이 page의 필요한 정보를 모두 갖는지 확인하지 못했다. 특정 호출 경로가 확인되기 전에는 `lower-layer-reverification` finding으로 확정하지 않는다.
- C++의 사용되지 않는 전파 대기 분기(`framework/languages/cpp/framework/src/runtime/host/app.cpp:3645`)는 삭제 후보다. `:3677`의 유일한 호출이 false이며 `framework/languages/cpp/framework/src/runtime/locations/location_runtime.hpp:72`의 legacy republish 함수는 항상 true다. 이를 실제 Store 게시 실패 retry로 보고하지 않았다.
- Provider CAS는 opaque bytes의 version 조건을 보장한다. ObjectGeneration·membership·inventory digest의 의미, target identity, 서로 다른 chunk의 관계는 Framework 책임이므로 해당 의미 검사를 일괄 제거하는 제안은 하지 않는다(`framework/doc/framework/common/spec/server/05-location-relocation/02-location-store-redis.ko.md:119`; `framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md:139`).
- Flow tracing의 `**언어별 재량**`은 방법의 차이와 Off 시 관찰 비용이 같아야 하는 이유를 이미 설명한다(`framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md:268`). 재량 문구 자체를 parity gap으로 분류하지 않았다.
- Metrics의 S0→S1, S2→S3, S1→S4는 host §8의 source/target-local 구간을 투영한다(`framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md:235`, `:250`, `:277`; `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:468`). 같은 1초라는 이유로 warning 목표와 cutover timeout을 합치지 않는다.

## 읽은 범위

정독 행 수는 중복해서 읽은 구간을 한 번만 센 값이다. 부분 읽기는 범위를 명시했다. 문자열 검색만 한 파일 전체를 정독으로 합산하지 않았다.

### 지정 spec 전체

| 파일 | 읽은 행 | 행 수 |
|---|---|---:|
| `framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md` | 1–1341 | 1,341 |
| `framework/doc/framework/common/spec/server/05-location-relocation/02-location-store-redis.ko.md` | 1–320 | 320 |
| `framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md` | 1–323 | 323 |
| `framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md` | 1–752 | 752 |
| `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md` | 1–981 | 981 |
| `framework/doc/framework/common/spec/server/05-location-relocation/06-failure-failover-policy.ko.md` | 1–313 | 313 |
| `framework/doc/framework/common/spec/server/05-location-relocation/README.ko.md` | 1–125 | 125 |
| `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md` | 1–446 | 446 |
| `framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md` | 1–375 | 375 |
| `framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md` | 1–349 | 349 |
| `framework/doc/framework/common/spec/server/06-observability/04-flow-correlation.ko.md` | 1–232 | 232 |
| `framework/doc/framework/common/spec/server/06-observability/README.ko.md` | 1–112 | 112 |
| 합계 | 생략 없음 | 5,669 |

### 지침·교차 참조·구현·gate의 부분 읽기

| 파일 | 읽은 행 | 중복 제외 행 수 |
|---|---|---:|
| `AGENTS.md` | 1–137 | 137 |
| `doc/AGENTS.md` | 1–51 | 51 |
| `doc/plan/c016-worklog/decisions.ko.md` | 1205–1212, 1235–1241, 1247–1251, 1256–1262, 1281–1292, 1302–1314, 1343–1378 | 88 |
| `doc/plan/c016-worklog/spec-review/README.ko.md` | 1–66 | 66 |
| `doc/principal/documentation/documentation-principles.ko.md` | 1–474 | 474 |
| `doc/principal/documentation/spec-writing-guide.ko.md` | 1–185, 336–418, 672–755 | 352 |
| `framework/AGENTS.md` | 1–128 | 128 |
| `framework/doc/AGENTS.md` | 1–30 | 30 |
| `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md` | 25–102 | 78 |
| `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md` | 169–192, 223–259, 280–286, 369–386 | 86 |
| `framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md` | 304–337 | 34 |
| `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md` | 435–530 | 96 |
| `framework/languages/cpp/framework/src/runtime/diagnostics/flow_context.hpp` | 75–104 | 30 |
| `framework/languages/cpp/framework/src/runtime/diagnostics/monitoring_runtime.cpp` | 324–349 | 26 |
| `framework/languages/cpp/framework/src/runtime/diagnostics/runtime_observation.hpp` | 143–165 | 23 |
| `framework/languages/cpp/framework/src/runtime/fanout/raw_fanout_owner.cpp` | 1–15, 359–440 | 97 |
| `framework/languages/cpp/framework/src/runtime/host/app.cpp` | 1368–1381, 3520–3572, 3580–3726 | 214 |
| `framework/languages/cpp/framework/src/runtime/locations/location_auto_connect_host_service.hpp` | 558–624 | 67 |
| `framework/languages/cpp/framework/src/runtime/locations/location_runtime.hpp` | 52–94 | 43 |
| `framework/languages/cpp/framework/src/runtime/locations/provider_relocation_repository.hpp` | 1–156 | 156 |
| `framework/languages/cpp/framework/src/runtime/locations/store_location_resolvers.hpp` | 1–175, 337–369 | 208 |
| `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp` | 788–813 | 26 |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` | 2757–2777, 3899–3929 | 52 |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp` | 74–110, 156–202 | 84 |
| `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.hpp` | 86–104 | 19 |
| `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp` | 6170–6293, 8590–8710, 8940–9055, 10330–10351, 11485–11505, 12702–12719 | 422 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkAutomaticFanoutSubscriberRuntime.cs` | 346–423 | 78 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkMessageFlowTracer.cs` | 32–72 | 41 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkObservationQueue.cs` | 58–105 | 48 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkDrainAdmissionGate.cs` | 53–179 | 127 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkDrainExecutor.cs` | 95–180, 505–542, 569–611 | 167 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkMaintenanceRuntime.cs` | 132–162, 696–719 | 55 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkAutoConnectReconciler.cs` | 198–301, 649–680, 729–756 | 164 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkProviderRelocationRepository.cs` | 65–131 | 67 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkRelocationTreeStore.cs` | 424–457 | 34 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/ZLinkStoreLocationResolvers.cs` | 155–223 | 69 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs` | 493–531 | 39 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs` | 8020–8065, 8068–8096, 8368–8391, 8460–8502 | 142 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotNodeInitializer.cs` | 15–39 | 25 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotRetireTransport.cs` | 279–354 | 76 |
| `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/SessionActorCoordinatorTests.cs` | 1310–1518 | 209 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java` | 6340–6374, 6903–6934 | 67 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/diagnostics/ZLinkMessageFlowTracer.java` | 46–80 | 35 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java` | 304–320, 2055–2138 | 101 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/drain/ZLinkMeshDrainCoordinator.java` | 1–109 | 109 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/locations/ZLinkProviderRelocationRepository.java` | 31–104 | 74 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/monitoring/ZLinkStatusPublisher.java` | 418–447 | 30 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkClassicFanoutLiveness.java` | 123–176 | 54 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkAutoConnectReconciler.java` | 32–91, 177–196 | 80 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/locations/ZLinkStoreLocationResolvers.java` | 1–180, 336–378 | 223 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java` | 191–254 | 64 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkCanonicalRelocationStateMachine.java` | 1–289 | 289 |
| `framework/languages/java/zlink-framework-kotlin/build.gradle.kts` | 1–22 | 22 |
| `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt` | 1–160 | 160 |
| `framework/languages/node/packages/framework/src/runtime/actors/deferred-join-accepted-journal.ts` | 960–1024 | 65 |
| `framework/languages/node/packages/framework/src/runtime/admission.ts` | 1–192 | 192 |
| `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts` | 365–383, 1510–1568 | 78 |
| `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts` | 545–626 | 82 |
| `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts` | 1025–1105, 1148–1172 | 106 |
| `framework/languages/node/packages/framework/src/runtime/diagnostics/runtime-observation-queue.ts` | 205–233 | 29 |
| `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts` | 145–205, 340–425, 665–787, 1244–1315 | 342 |
| `framework/languages/node/packages/framework/src/runtime/foundation/route-mesh-connection-policy.ts` | 1–28 | 28 |
| `framework/languages/node/packages/framework/src/runtime/foundation/service-topology-registry.ts` | 80–171 | 92 |
| `framework/languages/node/packages/framework/src/runtime/host/index.ts` | 1784–1809 | 26 |
| `framework/languages/node/packages/framework/src/runtime/host/route-mesh-runtime.ts` | 535–635 | 101 |
| `framework/languages/node/packages/framework/src/runtime/host/service-relocation-host-runtime.ts` | 2062–2200, 2374–2395 | 161 |
| `framework/languages/node/packages/framework/src/runtime/locations/auto-connect-reconciler.ts` | 350–400 | 51 |
| `framework/languages/node/packages/framework/src/runtime/locations/resolvers.ts` | 1–200, 401–441 | 241 |
| `framework/languages/node/packages/framework/src/runtime/streams/actor-session-binding-registry.ts` | 914–964, 1578–1618 | 92 |
| `framework/languages/node/test/contract/stream-runtime.test.js` | 4090–4108, 4160–4203, 4290–4327 | 101 |
| `framework/runtime/conformance/bound-session-relocation-v1.json` | 110–170 | 61 |
| `framework/runtime/conformance/relocation-conformance-adapters-v1.json` | 1–150, 241–267 | 177 |
| `framework/runtime/conformance/validate-runtime-conformance-fixtures.mjs` | 1436–1503, 3230–3320 | 159 |
| `scripts/run-framework-relocation-conformance.mjs` | 1–175 | 175 |

추가로 `rg`의 symbol 검색과 해당 `git log -S`·`git show` diff를 읽었다. Adapter evidence는 24개 문자열의 source 포함 여부만 정적으로 대조했고, 그 검색 대상 테스트 파일 전체를 위 정독 행 수에 포함하지 않았다. 현재 이름이 없던 항목은 .NET·Node 각각 하나였으며 나머지 문자열의 존재는 테스트의 실행·통과·계약 적합성을 뜻하지 않는다.

생략 범위와 이유: 지정된 두 spec 디렉터리의 한국어 파일은 생략하지 않았다. 영어 번역본·언어별 spec 전체·Core·binding 구현은 이번 R7의 지정 정독 범위가 아니어서 읽지 않았다. R4 오류 모델, R5 Session·liveness, R6 routing은 발견한 소유권 충돌에 필요한 구간만 읽었다. 각 언어 runtime 전체와 모든 Actor Join/host relocation 조합, conformance의 나머지 시나리오·E2E inventory는 관련 symbol·fixture 조건을 넘어 확대하지 않았다. Kotlin은 별도 경로가 없어 Java core와 Kotlin facade/dependency를 대조했다. Redis 실제 서버 응답, network 경쟁, observer overflow, malformed frame, shutdown failure는 실행 검증하지 않았다. 사용자 지시가 build·test·benchmark 실행을 금지하므로 runner의 `--list`도 실행하지 않았다.

## BLOCKERS

- **F-R7-5:** Relay-ready accepted 이후 source 재개를 전면 금지하는 §4.4·host·failover·Session 규칙과, source snapshot을 근거로 재개하는 relocation §9 중 어느 쪽을 계약으로 확정할 것인가? 현재 초안은 전자를 선택하며 C++ 행동 변경으로 분류했다.
- **F-R7-9 / R5:** Flow pair를 diagnostics와 무관하게 실패를 결정하는 필수 protocol 정보로 볼 것인가, message 결과를 바꾸지 않는 관측 정보로 볼 것인가? 어느 정의를 0.18.0 계약으로 확정할 것인가?
- **F-R7-7:** Host §16의 “Terminal event는 observer overflow로 잃지 않는다”는 공통 host status observer에 대한 예외 보장인가, 별도 structured log 표면만을 뜻하는가? 예외 보장이라면 bounded terminal 계약 중 어느 쪽을 유지할 것인가?
- **상태 표의 Completed:** Location §8.4의 `Completed`는 dispatch·Session update가 끝난 즉시 기록하는 충분조건인가, §9.3의 accepted request 완료·pending=0에 추가되는 필요조건 요약인가? 복구·보관 책임의 의미를 결정하지 않고 상태를 합칠 수 있는가?
- **F-R7-1 / R4·R6:** Owner unavailable의 공개 정책 소유자를 failure policy §4.2로 확정하고 routing §2.6과 error model을 투영으로 정리할 것인가? R4의 remote queue 설명은 이 owner 장애 의미를 제한하지 않는 문장으로 정리할 것인가?

이 보고서의 동작 변경 제안은 구현 승인이 아니다. 감독자가 계약을 선택하기 전에는 spec·runtime·fixture를 변경하지 않는다.
