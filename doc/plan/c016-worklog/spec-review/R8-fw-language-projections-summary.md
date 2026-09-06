# R8 — Framework 언어별 투영·HTTP client 심층 검토

| 번호 | 제목 | 분류 | 행동 변경 | 규칙 수 | 성능 영향 | 확신 |
|---|---|---|---|---|---|---|
| F-R8-1 | 관측 유실 counter의 공통 상한 소유자 | consolidation | 없음 | 5 → 1 | 없음 | 높음 |
| F-R8-2 | Actor create 불변 조건의 소유권 위임 충돌 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R8-3 | C++ async 명칭을 따라가지 못한 contract gate | gate-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R8-4 | command 44의 response·주기 재전송 잔재 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R8-5 | HTTP 의존 방향을 반대로 설명한 실행 문서 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R8-6 | NestJS inbound 설정 메서드의 오래된 복사본 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R8-7 | Kotlin generated JVM 선언이 생성 응답을 소실 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R8-8 | 금지된 HTTP Yield가 다섯 언어에 존재 | parity-gap | 있음 | 2 → 1 | 없음 | 높음 |
| F-R8-9 | HTTP timeout의 시도당·전체 operation 의미 충돌 | parity-gap | 있음 | 2 → 1 | 없음 | 높음 |
| F-R8-10 | C++ HTTP의 기본 동기 실행과 blocking fetch | parity-gap | 있음 | 3 → 1 | 있음 | 높음 |
| F-R8-11 | HTTP one-way가 response completion을 기다림 | spec-impl-drift | 있음 | 2 → 1 | 없음 | 높음 |
| F-R8-12 | Automatic·object MeshNode의 fixed RID 허용 충돌 | parity-gap | 있음 | 2 → 1 | 없음 | 높음 |
| F-R8-13 | C++ Spot timer에만 남은 raw monitoring 예외 | parity-gap | 있음 | 2 → 1 | 있음 | 높음 |
| F-R8-14 | Redis encoded blob에서 누락된 23 bytes | parity-gap | 있음 | 2 → 1 | 없음 | 높음 |
| F-R8-15 | Actor create 중복 옵션의 오류 분류 불일치 | parity-gap | 있음 | 3 → 1 | 없음 | 높음 |
| F-R8-16 | Kotlin·Java가 같은 create 제출 여부를 이중 소유 | lower-layer-reverification | 있음 | 2 → 1 | 있음 | 높음 |
| F-R8-17 | C++ session bind에 따라붙은 무효 옵션과 Yield | parity-gap | 있음 | 4 → 1 | 없음 | 높음 |
| F-R8-18 | Node HTTP typed status 실패의 잘못된 kind | parity-gap | 있음 | 2 → 1 | 없음 | 높음 |
| F-R8-19 | C++ SessionActorManager의 별도 local create | spec-impl-drift | 있음 | 2 → 1 | 없음 | 높음 |
| F-R8-20 | Cold activation 순서의 다섯 언어 재서술 | form | 없음 | 6 → 1 | 없음 | 높음 |

검토 branch는 `main`이다. 검토 중 조회한 HEAD `eea13092486de08a64cf7c3f8daba4f0587a0701`에서 다른 작업자의 변경으로 `efe0b621cc96ae39c72a6f86a00efc42d8678b94`까지 이동했다. 이 구간에서 R8 대상 언어·HTTP 한국어 문서는 바뀌지 않았으며, 인용한 공통 문서·구현의 변경 부분은 대조하고 이동한 행 번호를 반영했다. 작업 트리는 다른 작업자의 수정도 포함하므로 단일 commit의 clean snapshot을 검증했다는 뜻은 아니다. 본 작업은 스펙·구현·스크립트를 수정하지 않았으며 build, test, benchmark, contract gate를 실행하지 않았다. 아래 코드는 정적 호출 경로·공개 선언 근거이며 실행 성공을 뜻하지 않는다. 본 보고서 외의 작업 파일은 건드리지 않았다.

`규칙 수`는 해당 finding의 판정·예외·소유자 수다. 서로 다른 정책값을 통일하는 항목과 같은 규칙의 문서 소유자를 줄이는 항목은 각 본문에서 단위를 구분한다. `행동 변경: 없음`은 제안한 문서·gate 정정만 적용할 때의 판정이다. 런타임 수정 제안은 모두 별도 승인 대상이며 실제 변경은 하지 않았다. 본문 20개 중 행동 변경이 없는 8개는 문서·gate 정정 제안이고, 행동 변경이 있는 12개는 감독의 0.18.0 검토 대상이다.

### F-R8-1 관측 유실 counter의 공통 상한 소유자

- 분류: consolidation
- 위치: `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:314`, `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:325`; 구체 상한의 반복은 `framework/doc/framework/common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md:235`, `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:418`, `framework/doc/framework/common/spec/server/languages/java/interfaces/monitoring.ko.md:189`, `framework/doc/framework/common/spec/server/languages/node/interfaces/03-location-observability.ko.md:513`; Kotlin의 Java status 재사용은 `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.ko.md:5`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.ko.md:85`.
- 현재 규칙(인용): 공통은 “표현 범위를 넘으면 최댓값으로 고정한다.”이고 C++는 “`9223372036854775807`(`2^63 - 1`)을 넘으면 그 값으로 고정한다.”이다.
- 문제: 공통 문서에 없는 숫자 상한을 네 언어 문서가 각각 결정한다. C++ `uint64_t`, .NET `ulong`, Java `long`, Node `bigint`의 표현 범위는 서로 다르므로 공통 문장의 ‘표현 범위’만으로 같은 상한이 나오지 않는다. 구현은 네 runtime 모두 signed-64 최댓값에서 포화하며 Kotlin은 Java 값 자체를 전달한다. 따라서 새 동작을 정할 문제가 아니라 이미 일치하는 관찰 규칙의 소유자를 정할 문제다.
- 제안: 공통 Runtime monitoring §7.2에 “두 관측 유실 counter는 구독마다 0에서 시작하고 각각 단조 증가하다 `2^63 - 1`에서 포화하며 언어의 정수 표현형은 이 관찰 범위를 바꾸지 않는다.”를 두고 언어 문서의 상한 설명은 이 절을 링크한다.
- 규칙 수: before 5 → after 1 — 공통의 추상 상한 설명과 네 언어의 구체 상한 소유자를 하나로 합친다; Kotlin은 추가 소유자가 아니다.
- 행동 변경: 없음 — 실제 counter 값·포화 경계·공개 정수형을 바꾸지 않는다.
- 영향: framework(cpp, dotnet, java, kotlin, node) — 아래 counter와 projection의 문서 소유권 정리다.
- 성능 영향: 없음 — counter 갱신·queue·allocation을 바꾸지 않는다.
- 근거 코드: C++ `framework/languages/cpp/framework/include/zlink/framework/contracts/monitoring/framework_runtime.hpp:37`, `framework/languages/cpp/framework/src/runtime/diagnostics/runtime_observation.hpp:26`; .NET `framework/languages/dotnet/src/Zlink.Framework/Contracts/Configuration/ZLinkDrainContracts.cs:80`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkObservationQueue.cs:16`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkObservationQueue.cs:198`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/monitoring/ZLinkObservationLoss.java:4`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/monitoring/ZLinkStatusPublisher.java:425`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/monitoring/ZLinkStatusPublisher.java:462`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkPublisherFlowBridge.kt:18`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkPublisherFlowBridge.kt:23` 및 같은 Java counter; Node `framework/languages/node/packages/framework/src/contracts/RouteMesh/Contracts.ts:127`, `framework/languages/node/packages/framework/src/runtime/diagnostics/runtime-observation-queue.ts:7`, `framework/languages/node/packages/framework/src/runtime/diagnostics/runtime-observation-queue.ts:53`.
- 확신: 높음 — 다섯 언어의 값 표현과 실제 포화 경계를 확인했다. Stream 전체의 성능·동시성 검증은 실행하지 않았다.

### F-R8-2 Actor create 불변 조건의 소유권 위임 충돌

- 분류: consolidation
- 위치: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:40`; 이미 공통 소유자인 `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:396`, `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:411`; 투영은 `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:253`, `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/06-actors.ko.md:312`, `framework/doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md:272`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.ko.md:20`, `framework/doc/framework/common/spec/server/languages/node/interfaces/05-actors.ko.md:134`.
- 현재 규칙(인용): Submit 문서는 “Single-use 여부, 같은 option을 반복했을 때의 처리와 terminal 재호출 오류는 각 operation의 언어별 interface가 정의한다.”라고 한다. Actor 모델은 “같은 option을 두 번 설정하면 `InvalidOperation`이다.”라고 이미 정한다.
- 문제: Actor create의 관찰 가능한 불변 조건을 공통 operation 문서와 언어별 문서가 동시에 소유하게 하는 위임이다. 다섯 언어 문서가 같은 규칙을 반복하므로 언어 재량의 근거도 없다. 문서 소유권 문제와 실제 오류 분류 결함(F-R8-15)은 분리해야 한다.
- 제안: Submit §2의 위임 경계에 “Actor Create/GetOrCreate의 single-use, 중복 옵션과 재제출 오류는 Actor 모델 §6.2가 소유하며 언어별 interface는 이름과 타입만 투영한다.”를 두고 각 투영은 해당 절을 링크한다.
- 규칙 수: before 2 → after 1 — Actor create에 대한 공통 operation 소유권과 언어별 재정의 권한을 하나로 합친다.
- 행동 변경: 없음 — 기존 Actor 모델의 `InvalidOperation` 계약을 변경하지 않는다; 구현 오류 수정은 F-R8-15다.
- 영향: framework(cpp, dotnet, java, kotlin, node) — create call의 공통 계약 출처를 명확히 한다.
- 성능 영향: 없음 — 실행 경로를 바꾸지 않는다.
- 근거 코드: C++ `framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:103`, `framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:156`; .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:732`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:743`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2632`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2661`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:321`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:330`; Node `framework/languages/node/packages/framework/src/runtime/actors/index.ts:899`, `framework/languages/node/packages/framework/src/runtime/actors/index.ts:917`.
- 확신: 높음 — 공통 operation에 규칙이 이미 존재하며 다섯 언어의 공개 call과 판정 경로를 대조했다.

### F-R8-3 C++ async 명칭을 따라가지 못한 contract gate

- 분류: gate-drift
- 위치: 명칭 소유자는 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:50`, `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:498`; 검사 대상 선언은 `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:711`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:717`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:732`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:747`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:974`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:983`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:1003`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:1009`.
- 현재 규칙(인용): 공통 명칭은 “.NET `Async`, C++ `async`, Java·Node.js `submit`, Kotlin 전용 wrapper의 `await`”인데 script는 `cpp: /task_t<void>\s+submit\s*\(\s*\)/`를 검사한다.
- 문제: Drift의 소유자는 **script**다. `cfa70e8a681b7f6d653329e1c1aef244298e3a46`은 C++ 문서와 public call 구현을 함께 `async`로 바꿨으며 이 script는 그 commit의 변경 대상이 아니다. 현재 검사 대상 파일의 여덟 one-way 선언은 모두 `task_t<void> async();`다. 이전 이름을 검사하므로 `cpp async-only submit projection is missing` 분기로 가는 정적 원인이 확인된다. 이 결론을 위해 gate를 실행하지 않았다.
- 제안: 공통 Submit §2·§16을 단일 소유자로 삼아 “C++ 비동기 one-way terminal은 `task_t<void> async()`이며 contract gate도 이 투영을 검사한다.”로 일치시킨다.
- 규칙 수: before 2 → after 1 — 계약의 `async`와 gate의 오래된 `submit` 판정을 하나로 맞춘다.
- 행동 변경: 없음 — 문서나 public API를 이전 이름으로 되돌리지 않고 검사식만 정정하는 제안이다.
- 영향: framework(cpp) — `scripts/verify-framework-submit-api.sh:68`, `scripts/verify-framework-submit-api.sh:70`, `scripts/verify-framework-submit-api.sh:78`.
- 성능 영향: 없음 — runtime 경로가 아니다.
- 근거 코드: C++ `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:272`, `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:423`; 검사 코드 `scripts/verify-framework-submit-api.sh:65`, `scripts/verify-framework-submit-api.sh:75`. 다른 네 언어 gate의 실행 결과는 검증하지 않았다.
- 확신: 높음 — commit 이력, 현재 검사식, 검사 대상의 선언과 public 구현이 일치하는 정적 진단이다. 전체 gate의 후속 실패 유무는 미검증이다.

### F-R8-4 command 44의 response·주기 재전송 잔재

- 분류: spec-impl-drift
- 위치: 소유 규칙은 `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:508`, `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:524`, `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:530`, `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:742`, `framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:453`, `framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:567`, `framework/doc/framework/common/spec/server/03-spot-actor/01-spot-model.ko.md:408`, `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:579`, `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:683`. 오래된 설명은 C++ `framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md:7`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md:699`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:8`; .NET `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.ko.md:391`, `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/06-actors.ko.md:264`; Java `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:7`, `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:236`, `framework/doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md:7`, `framework/doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md:69`, `framework/doc/framework/common/spec/server/languages/java/interfaces/stream-session.ko.md:35`; Kotlin `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:8`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:381`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.ko.md:8`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/stream-session.ko.md:31`; Node `framework/doc/framework/common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md:368`, `framework/doc/framework/common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md:407`, `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:117`, `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:453`, `framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md:8`, `framework/doc/framework/common/spec/server/languages/node/interfaces/05-actors.ko.md:8`. 이미 올바른 투영은 `framework/doc/framework/common/spec/server/languages/cpp/interfaces/06-stream-session.ko.md:188`, `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md:178`이다.
- 현재 규칙(인용): 오래된 투영은 “응답이 없어도 Actor 처리를 멈추지 않으며 정해진 간격으로 같은 요청을 다시 보낸다.”이고 일부는 `sessionActorLocationUpdateResMsg`를 요구한다. C++ STREAM 투영은 “one-way command로 한 번만 send하며 재전송하지 않는다”라고 한다.
- 문제: command 44는 현재 공통 계약에서 response 없는 route commit/abort control이다. Seal request/ack인 42/43과 별개다. 확인한 구현은 route record를 한 번 제출하고 response를 기다리거나 주기적으로 다시 보내지 않는다. 같은 언어 디렉터리 안에도 예전 request/response 설명과 현재 one-way 설명이 공존한다. 이 잔재를 근거로 retry를 복원하면 현행 계약을 위반한다.
- 제안: Session–Actor binding §8.2에 “command 44의 route commit/abort는 response 없이 한 번 제출하고 Session owner가 binding route와 current ActorRef 위치 snapshot을 함께 갱신하며 언어별 interface는 이 결과만 투영한다.”를 단일 규칙으로 두고 response·주기 재전송 설명을 삭제한다.
- 규칙 수: before 2 → after 1 — response/retransmission 정책과 one-way 정책 가운데 현재 공통 정책 하나만 남긴다.
- 행동 변경: 없음 — 제안은 오래된 문서 설명만 정정하며 현재 전송·수신 동작은 바꾸지 않는다.
- 영향: framework(cpp, dotnet, java, kotlin, node) — 세션 route 갱신의 언어별 설명이다.
- 성능 영향: 없음 — 현재 구현에 문서의 주기 재전송을 추가하거나 제거하는 작업이 아니다.
- 근거 코드: C++ `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.hpp:992`, `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:2953`, `framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:2991`; .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:1214`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:1245`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionRelocationPeerClient.java:38`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:3371`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:98`와 Java의 앞 두 route 구현을 공유한다; Node `framework/languages/node/packages/framework/src/runtime/host/service-relocation-host-runtime.ts:1016`, `framework/languages/node/packages/framework/src/runtime/host/service-relocation-host-runtime.ts:1035`. 이는 command 44의 제출 경로 대조이며 relocation 전체 성공을 실행 검증한 것은 아니다.
- 확신: 높음 — 다섯 언어의 실행 경로와 command 42/43·44의 책임 차이를 확인했다.

### F-R8-5 HTTP 의존 방향을 반대로 설명한 실행 문서

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:71` 대 `framework/doc/framework/common/spec/http-client/01-scope-and-architecture.ko.md:39`, `framework/doc/framework/common/spec/http-client/01-scope-and-architecture.ko.md:42`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:30`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:101`; 언어별 설명은 `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:26`, `framework/doc/framework/common/spec/http-client/languages/dotnet/dotnet-http-client.ko.md:16`, `framework/doc/framework/common/spec/http-client/languages/dotnet/dotnet-http-client.ko.md:30`, `framework/doc/framework/common/spec/http-client/languages/java/java-http-client.ko.md:16`, `framework/doc/framework/common/spec/http-client/languages/kotlin/kotlin-http-client.ko.md:14`, `framework/doc/framework/common/spec/http-client/languages/node/node-http-client.ko.md:16`.
- 현재 규칙(인용): 실행 문서는 “바이너리 의존은 `framework → HTTP client` 한 방향을 유지한다.”라고 하고 정본은 “**의존은 한 방향이다: HTTP client → framework 계약.**”이라고 한다.
- 문제: 현재 package 선언과 public 오류·task 타입은 HTTP client가 Framework를 소비하는 방향이다. 실행 scheduler를 주입한다는 사실을 binary dependency의 반대 방향으로 설명한 것이 원인이다. .NET은 별도 Contracts package를 쓰고 Java·C++는 더 큰 Framework target을 사용하므로, 방향이 같다는 사실을 package 크기까지 같다는 주장으로 확대하면 안 된다.
- 제안: HTTP scope §1.3에 “HTTP client는 Framework 공용 오류·codec 계약을 소비하고 Framework core는 HTTP client 없이 동작하며 server turn 연결은 integration의 execution scheduler 주입점으로 제공한다.”를 두고 §5.3의 반대 방향 설명을 삭제한다.
- 규칙 수: before 2 → after 1 — 반대 방향 두 설명을 하나로 맞춘다.
- 행동 변경: 없음 — 의존성을 바꾸거나 package를 이동하는 제안이 아니다.
- 영향: framework(cpp, dotnet, java, kotlin, node) — HTTP architecture 설명의 정정이다.
- 성능 영향: 없음 — runtime 경로를 바꾸지 않는다.
- 근거 코드: C++ `framework/languages/cpp/CMakeLists.txt:338`, `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:6`; .NET `framework/languages/dotnet/src/Zlink.HttpClient/Zlink.HttpClient.csproj:16`, `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:55`; Java `framework/languages/java/zlink-http-client/build.gradle.kts:19`, `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:187`; Kotlin `framework/languages/java/zlink-http-client-kotlin/build.gradle.kts:20`, `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:40`; Node `framework/languages/node/packages/http-client/package.json:20`, `framework/languages/node/packages/http-client/src/request-builder.ts:3`.
- 확신: 높음 — 다섯 언어의 package 의존과 실제 사용하는 Framework 타입을 확인했다; 각 package의 의존 최소화 여부는 별도 설계 검토다.

### F-R8-6 NestJS inbound 설정 메서드의 오래된 복사본

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md:169`, `framework/doc/framework/common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md:410`; 올바른 기본 투영은 `framework/doc/framework/common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md:475`, `framework/doc/framework/common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md:491`, `framework/doc/framework/common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md:497`; 소유 계약은 `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:25`, `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:65`.
- 현재 규칙(인용): NestJS 문서는 “`configureDispatch()`가 반환하는 `ZLinkDispatchOptionsBuilder`의 `coreHwmMemoryLimitBytes`, `coreHwmBudgetBytes`, `coreHwmProfile`”이라고 한다.
- 문제: 현재 NestJS public interface에는 `configureInboundDispatch()`가 있고 HWM·job queue 설정은 이 반환형에 있다. 문서의 signature 복사본은 이 메서드를 누락하고 diagnostics용 `configureDispatch()`에 필드를 잘못 귀속시킨다. Core와 Framework capacity의 의미를 NestJS adapter에서 다시 설명한 부분도 변경에 뒤처졌다.
- 제안: Node foundation의 설정 투영을 소유자로 삼아 “NestJS의 `configureInboundDispatch(): ZLinkInboundDispatchOptions`는 기본 Framework의 inbound 설정 투영을 그대로 제공하며 HWM·job queue 의미는 공통 backpressure 계약을 따른다.”로 연결하고 잘못된 중복 필드 설명을 삭제한다.
- 규칙 수: before 2 → after 1 — 같은 설정 표면의 base·NestJS 복사본 중 의미 소유자를 하나로 둔다.
- 행동 변경: 없음 — 이미 존재하는 NestJS 메서드를 문서에 정확히 반영하는 제안이다.
- 영향: framework(node) — NestJS 공개 옵션 투영이다. 다른 언어에는 이 NestJS interface가 없으므로 해당 없음이다.
- 성능 영향: 없음 — options builder나 Core 설정 전달을 변경하지 않는다.
- 근거 코드: Node `framework/languages/node/packages/nestjs/src/contracts.ts:198`, `framework/languages/node/packages/nestjs/src/contracts.ts:199`, `framework/languages/node/packages/nestjs/src/options-builder.ts:159`, `framework/languages/node/packages/nestjs/src/options-builder.ts:180`, `framework/languages/node/packages/framework/src/contracts/Configuration/RegistrationBuilders.ts:141`.
- 확신: 높음 — 공개 interface와 두 반환 builder의 실제 생성 경로를 확인했다.

### F-R8-7 Kotlin generated JVM 선언이 생성 응답을 소실

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:205`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:308`; 상위 JVM 계약은 `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:141`, `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:454`; 공통 결과는 `framework/doc/framework/common/spec/server/03-spot-actor/01-spot-model.ko.md:184`, `framework/doc/framework/common/spec/server/03-spot-actor/01-spot-model.ko.md:244`, `framework/doc/framework/common/spec/server/03-spot-actor/01-spot-model.ko.md:248`; 두 표현의 대응 의무는 `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/README.ko.md:44`.
- 현재 규칙(인용): Kotlin source는 `): ZLinkActorCreateResponse`이고 같은 파일의 generated JVM 표는 `java.util.concurrent.CompletionStage<java.lang.Void> onCreateActor(...)`다.
- 문제: 수동으로 보관한 generated 선언이 source 계약과 실제 bridge 반환형을 따라가지 못했다. 승인·거절과 optional reply는 공통 계약의 관찰 결과이므로 `Void`로 바꾸는 것이 해결책이 아니다. ‘generated JVM signature’는 실제 생성 결과를 설명하는 재현 자료이며 독립된 두 번째 반환형 계약이 될 수 없다.
- 제안: Kotlin Spot source 투영을 소유자로 삼아 “`onCreateActorSuspending`의 생성 응답은 JVM bridge에서도 `CompletionStage<ZLinkActorCreateResponse>`로 보존하며 generated inventory는 이 source와 Java interface를 그대로 투영한다.”로 통일한다.
- 규칙 수: before 2 → after 1 — source 반환형과 따로 관리한 JVM 반환형 판정을 하나로 맞춘다.
- 행동 변경: 없음 — 구현·ABI를 변경하지 않고 잘못된 inventory를 정정하는 제안이다.
- 영향: framework(kotlin, java) — Kotlin bridge와 상위 Java interface의 문서 대응이다. C++·.NET·Node에는 이 JVM bridge가 없으므로 해당 없음이다.
- 성능 영향: 없음 — callback이나 coroutine 경로를 변경하지 않는다.
- 근거 코드: Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/contracts/ZLinkSuspendingHandlers.kt:252`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/contracts/ZLinkSuspendingHandlers.kt:255`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/spots/ZLinkEntrySpot.java:28`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/spots/ZLinkEntrySpot.java:31`.
- 확신: 높음 — source의 명시 반환형을 확인했다. `javap`나 build를 새로 실행하지 않았다.

### F-R8-8 금지된 HTTP Yield가 다섯 언어에 존재

- 분류: parity-gap
- 위치: 허용 family의 공통 소유자는 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:55`, `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:62`; HTTP 금지는 `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:68`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:85`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:96`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:108`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:164`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:166`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:167`, `framework/doc/framework/common/spec/http-client/language-interfaces.ko.md:67`, `framework/doc/framework/common/spec/http-client/languages/dotnet/dotnet-http-client.ko.md:92`, `framework/doc/framework/common/spec/http-client/languages/java/java-http-client.ko.md:60`, `framework/doc/framework/common/spec/http-client/languages/kotlin/kotlin-http-client.ko.md:63`. 반대 투영은 `framework/doc/framework/common/spec/http-client/README.ko.md:18`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:16`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:47`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:51`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:78`, `framework/doc/framework/common/spec/http-client/language-interfaces.ko.md:64`, `framework/doc/framework/common/spec/http-client/languages/dotnet/dotnet-http-client.ko.md:64`, `framework/doc/framework/common/spec/http-client/languages/kotlin/kotlin-http-client.ko.md:45`, `framework/doc/framework/common/spec/http-client/languages/kotlin/kotlin-http-client.ko.md:62`, `framework/doc/framework/common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md:369`.
- 현재 규칙(인용): 정본은 “DI와 단독 사용 모두 HTTP request builder에 `Yield`를 노출하지 않는다.”라고 하지만 공통 언어 표에는 “**gate 반납 완료** (서버 builder 전용)” 행에 다섯 언어의 Yield가 있다.
- 문제: 같은 HTTP 문서 안에서 허용과 금지가 충돌하고 실제 C++·.NET·Java·Kotlin·Node server builder 모두 Yield를 제공한다. 따라서 금지 문장에 맞추는 작업은 documentation-only가 아니다. ‘단독 사용에는 gate가 없다’는 이유는 standalone과 server의 차이는 설명하지만 공통 계약이 제외한 HTTP operation을 server에서 허용할 근거가 되지 않는다.
- 제안: 공통 Submit §2를 family 소유자로 유지하고 HTTP §3에는 “HTTP request builder는 response completion 동안 현재 turn을 유지하며 HTTP 전용 Yield를 제공하지 않고 gate 반납은 공통 실행 계약의 허용 operation만 수행한다.”를 두어 현재 금지 계약으로 통일한다.
- 규칙 수: before 2 → after 1 — HTTP Yield 허용·금지 정책을 하나로 만든다.
- 행동 변경: 있음 — 이미 노출된 `Yield`·`yield`·`yieldRaw`를 쓰는 application의 호출 가능성과 실행 순서가 달라진다. 금지 계약을 유지할지에 대한 감독 결정을 먼저 받아야 한다.
- 영향: framework(cpp, dotnet, java, kotlin, node) — 다섯 server HTTP request builder다.
- 성능 영향: 없음 — 특정 hot path 최적화를 주장하지 않는다; gate 반납 가능성의 변경은 실행 의미 변경이다.
- 근거 코드: C++ `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:342`, `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:366`; .NET `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:518`, `framework/languages/dotnet/src/Zlink.Framework.AspNetCore/ZLinkSpotHttpExecutionScheduler.cs:13`; Java `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpServerRequestBuilder.java:50`, `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkFrameworkHttpExecutionTurn.java:14`; Kotlin `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:71`와 Java `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpServerRequestBuilder.java:50`; Node `framework/languages/node/packages/nestjs/src/http-client-module.ts:25`, `framework/languages/node/packages/http-client/src/request-builder.ts:356`.
- 확신: 높음 — 다섯 언어 모두 공개 선언뿐 아니라 gate 연결 경로가 존재한다. 실제 scheduling 시나리오는 실행하지 않았다.

### F-R8-9 HTTP timeout의 시도당·전체 operation 의미 충돌

- 분류: parity-gap
- 위치: `framework/doc/framework/common/spec/http-client/06-redirect-retry-cookie.ko.md:37`, `framework/doc/framework/common/spec/http-client/06-redirect-retry-cookie.ko.md:40`, `framework/doc/framework/common/spec/http-client/03-request-builder.ko.md:29`, `framework/doc/framework/common/spec/http-client/09-error-model.ko.md:14`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:155`; C++ 경계는 `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:249`; 비계약 이력은 `framework/doc/framework/common/spec/http-client/10-revision-candidates.ko.md:5`, `framework/doc/framework/common/spec/http-client/10-revision-candidates.ko.md:11`.
- 현재 규칙(인용): “timeout은 **시도(attempt)당** 적용한다.”, “재시도 전체를 아우르는 총 데드라인은 계약에 없다”라고 하면서 C++ coroutine의 전체 deadline을 언어 편차로 적었다. 비계약 표는 “cpp만 두 경로 모두 총 예산 강제”라고 한다.
- 문제: 현재 C++는 동기·coroutine 두 경로 모두 동일 timeout에서 절대 deadline을 만들고 retry·지연 전체에 남은 시간을 적용한다. .NET·Java·Node는 attempt를 시작할 때 timeout 경계를 다시 만들며 Kotlin은 Java 경로를 사용한다. 같은 timeout과 retry 횟수에서도 최종 완료 시각과 실행되는 attempt 수가 다르므로 동등한 언어 재량이 아니다. C++ 동기 경로에 관한 §6.2의 구현 설명도 뒤처져 있다. 비계약 R3′는 어느 쪽을 공통 계약으로 승격할 권한을 주지 않는다.
- 제안: HTTP retry §6.2를 단일 소유자로 삼아 “설정된 HTTP timeout은 각 전송 attempt에 적용하며 retry 횟수·지연은 기존 HTTP retry 계약을 따르고 언어별로 별도 총 deadline을 합성하지 않는다.”로 현재 계약을 일치시킨다.
- 규칙 수: before 2 → after 1 — 같은 timeout 값의 적용 범위인 attempt·전체 operation 두 정책을 하나로 맞춘다.
- 행동 변경: 있음 — C++의 완료 시점과 수행 가능한 attempt가 달라진다. Timeout 값이나 retry 횟수를 늘리는 완화 제안이 아니며, R3′의 승격 여부는 감독 판단으로 남긴다.
- 영향: framework(cpp, dotnet, java, kotlin, node) — C++의 실제 적용 범위를 다른 네 언어 및 현재 계약과 대조한 결과다.
- 성능 영향: 없음 — timer·retry 자원 감소를 주장하지 않는다. 이 항목은 시간 계약의 차이다.
- 근거 코드: C++ `framework/languages/cpp/http-client/src/runtime/http_client_runtime.cpp:30`, `framework/languages/cpp/http-client/src/runtime/http_client_runtime.cpp:45`, `framework/languages/cpp/http-client/src/runtime/http_client_runtime.cpp:75`; .NET `framework/languages/dotnet/src/Zlink.HttpClient/Runtime/RetryPolicy.cs:30`, `framework/languages/dotnet/src/Zlink.HttpClient/Runtime/RetryPolicy.cs:85`; Java `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/internal/RetryPolicy.java:44`, `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/internal/RequestPerformer.java:67`; Kotlin `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:40`와 Java의 앞 두 runtime 경로; Node `framework/languages/node/packages/http-client/src/runtime/retry-policy.ts:30`, `framework/languages/node/packages/http-client/src/runtime/retry-policy.ts:38`.
- 확신: 높음 — deadline을 만드는 위치와 retry 반복 경계를 네 runtime 및 Kotlin bridge에서 확인했다.

### F-R8-10 C++ HTTP의 기본 동기 실행과 blocking fetch

- 분류: parity-gap
- 위치: 금지는 `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:114`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:164`, `framework/doc/framework/common/spec/http-client/04-response-model.ko.md:16`, `framework/doc/framework/common/spec/http-client/language-interfaces.ko.md:65`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:90`; 예외는 `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:100`, `framework/doc/framework/common/spec/http-client/language-interfaces.ko.md:73`, `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:65`, `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:111`, `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:123`, `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:141`, `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:205`, `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:289`. 다른 언어의 비동기 투영은 `framework/doc/framework/common/spec/http-client/languages/dotnet/dotnet-http-client.ko.md:96`, `framework/doc/framework/common/spec/http-client/languages/java/java-http-client.ko.md:44`, `framework/doc/framework/common/spec/http-client/languages/java/java-http-client.ko.md:56`, `framework/doc/framework/common/spec/http-client/languages/kotlin/kotlin-http-client.ko.md:39`, `framework/doc/framework/common/spec/http-client/languages/kotlin/kotlin-http-client.ko.md:54`, `framework/doc/framework/common/spec/http-client/languages/node/node-http-client.ko.md:40`, `framework/doc/framework/common/spec/http-client/languages/node/node-http-client.ko.md:55`.
- 현재 규칙(인용): 정본은 “같은 의미의 blocking 대안 terminator는 계약 위반이다.”라고 하지만 C++는 “coroutine scheduler: 설정하지 않은 client는 기존 blocking submit 의미를 유지한다.”라고 한다.
- 문제: C++ `fetch<T>()`는 `.result().value()`를 호출하며 server builder도 base를 public 상속한다. 또한 coroutine 설정이 없는 `submit_raw()`는 runtime `execute()`를 호출한 뒤 완료 task를 만든다. 따라서 반환형이 `task_t`라는 것만으로 non-blocking 투영이 되지 않는다. 다른 네 언어의 fetch는 awaitable/suspend를 유지한다. CLI 예외와 opt-in coroutine 경로라는 두 추가 규칙 때문에 같은 HTTP API의 thread 점유 의미가 달라진다.
- 제안: HTTP §3.3에 “HTTP 전송 terminal과 decoded-body 편의 terminal은 모두 비동기로 완료하며 동기 대기는 필요할 때 caller가 언어의 표준 awaitable 처리로 구성한다.”를 단일 규칙으로 두어 기존 비동기 경로로 통일한다.
- 규칙 수: before 3 → after 1 — 기본 inline 전송, blocking fetch, coroutine opt-in이라는 실행 선택 규칙을 비동기 완료 규칙 하나로 줄인다.
- 행동 변경: 있음 — C++ `fetch`의 공개 반환형과 기본 submit의 thread 점유·완료 시점이 달라질 수 있다.
- 영향: framework(cpp); 비교 기준은 dotnet, java, kotlin, node의 비동기 fetch다.
- 성능 영향: 있음 — C++ HTTP submit 호출 경로의 `execute()` 동기 대기와 fetch의 caller-thread blocking wait를 없애는 방향이다. Transport worker 점유나 처리량 개선 수치는 측정하지 않았다.
- 근거 코드: C++ `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:223`, `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:263`, `framework/languages/cpp/http-client/src/client.cpp:563`; .NET `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:192`, `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:198`; Java `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:167`, `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:172`; Kotlin `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:40`, `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:49`; Node `framework/languages/node/packages/http-client/src/request-builder.ts:203`, `framework/languages/node/packages/http-client/src/request-builder.ts:242`.
- 확신: 높음 — C++의 blocking 경로와 다른 네 언어의 실제 반환·대기 형태를 확인했다.

### F-R8-11 HTTP one-way가 response completion을 기다림

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:64`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:73`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:74`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:13`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:22`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:23`, `framework/doc/framework/common/spec/http-client/language-interfaces.ko.md:62`, `framework/doc/framework/common/spec/http-client/language-interfaces.ko.md:69`; 언어별 one-way 투영은 `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:66`, `framework/doc/framework/common/spec/http-client/languages/dotnet/dotnet-http-client.ko.md:61`, `framework/doc/framework/common/spec/http-client/languages/java/java-http-client.ko.md:46`, `framework/doc/framework/common/spec/http-client/languages/kotlin/kotlin-http-client.ko.md:43`, `framework/doc/framework/common/spec/http-client/languages/node/node-http-client.ko.md:41`; 일반 완료 원칙은 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:46`, `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:100`.
- 현재 규칙(인용): one-way는 “HTTP 요청이 전송 경계에 제출될 때까지 기다린다”, response completion은 “HTTP response가 도착할 때까지 기다린다”로 구분한다.
- 문제: 다섯 언어의 server one-way는 raw response operation을 시작하고 그 완료를 기다린 다음 결과만 버린다. 별도 admission 완료를 반환하는 구현이 아니다. .NET은 이 대기 안의 실패를 runtime error 경계로 보고하고 나머지 경로는 raw operation의 완료를 그대로 연결한다. HTTP 응답이 지연될 때 one-way의 완료도 지연되므로 두 완료 축의 이름만 다르고 기다리는 경계는 같아진다. 다만 스펙의 ‘전송 경계’가 transport queue 수락인지 실제 request 전송 완료인지는 충분히 고정되어 있지 않다.
- 제안: HTTP §3의 완료 경계를 단일 소유자로 삼아 “HTTP one-way는 HTTP transport가 request의 전송 admission을 확정했을 때 완료하고 response 수신·body 소비 완료는 response-completion terminal만 기다린다.”로 구분하되 admission의 공개 관찰 기준은 감독이 확정한다.
- 규칙 수: before 2 → after 1 — one-way 완료에 대해 문서의 admission 판정과 구현의 response 판정 중 하나만 남긴다; response-completion operation 자체를 없애는 제안은 아니다.
- 행동 변경: 있음 — 응답을 지연시키는 endpoint에 대한 one-way 완료 시점과 실패가 전달되는 경계가 달라진다.
- 영향: framework(cpp, dotnet, java, kotlin, node) — 공통적으로 raw response 완료를 재사용하는 server one-way다.
- 성능 영향: 없음 — 이 진단은 완료 의미에 관한 것이며 새 queue·worker·poller나 처리량 개선을 제안하지 않는다.
- 근거 코드: C++ `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:326`, `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:377`; .NET `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:266`, `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:507`; Java `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpServerRequestBuilder.java:31`, `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:157`; Kotlin `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:63`와 Java `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpServerRequestBuilder.java:31`; Node `framework/languages/node/packages/http-client/src/request-builder.ts:173`, `framework/languages/node/packages/http-client/src/request-builder.ts:345`.
- 확신: 높음 — 다섯 언어가 response operation을 기다린다는 사실은 확인했다. 새 admission 관찰 기준은 미확정이므로 BLOCKERS에 남긴다.

### F-R8-12 Automatic·object MeshNode의 fixed RID 허용 충돌

- 분류: parity-gap
- 위치: 제한 규칙은 `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:103`, `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:466`, `framework/doc/framework/common/spec/server/02-channel-transport/04-network-listener-identity.ko.md:308`, `framework/doc/framework/common/spec/server/02-channel-transport/04-network-listener-identity.ko.md:383`, `framework/doc/framework/common/spec/server/02-channel-transport/01-channel-topology.ko.md:60`, `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:1043`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:432`; 허용 규칙은 `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/03-configuration-topology.ko.md:356`, `framework/doc/framework/common/spec/server/languages/java/interfaces/configuration-host.ko.md:389`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/configuration-host.ko.md:202`.
- 현재 규칙(인용): 공통은 “Object role이 `Client` 또는 `Server`인 MeshNode에 fixed RID를 설정하거나 automatic mode와 fixed RID를 함께 설정하면 startup configuration error다.”이고 .NET·Java·Kotlin은 “Fixed RID는 automatic discovery topology에서도, object role이 있는 MeshNode에서도 허용한다.”이다.
- 문제: 같은 구성의 startup 성공·실패를 정반대로 요구한다. .NET validator는 허용 정책을 명시하며 공통 문서 대신 .NET 언어 문서를 근거로 주석에 인용한다. ‘시험 시나리오에서 peer를 이름으로 지목’해야 한다는 이유는 관찰 결과가 같다는 재량 근거가 아니며 공개 허용 범위의 변경이다. C++·Java·Node의 설정 저장·검증 경로도 읽었지만 모든 startup validator를 끝까지 실행하거나 전수 추적하지 않았으므로 다섯 언어가 실제 startup에서 동일하다고 주장하지 않는다.
- 제안: MeshNode §3.3을 유일한 소유자로 삼아 “Fixed RID는 Location descriptor·automatic discovery·object role을 사용하지 않는 explicit manual MeshNode에서만 허용한다.”로 현재 공통 계약을 통일하는 안을 감독에게 제시한다.
- 규칙 수: before 2 → after 1 — 같은 구성의 허용·거부 두 규칙을 하나로 확정한다.
- 행동 변경: 있음 — 현재 허용 구현을 공통 제한에 맞추면 application 구성의 startup 결과가 달라진다; 허용 쪽 승격도 계약 변경이므로 문서 정정으로만 처리하면 안 된다.
- 영향: framework(cpp, dotnet, java, kotlin, node) — .NET 허용 판정은 명시적으로 확인했고 다른 runtime의 최종 startup 결과는 미검증이다.
- 성능 영향: 없음 — startup 구성 계약이며 hot path 변경이 아니다.
- 근거 코드: .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Configuration/ZLinkSpotRegistrationValidator.cs:39`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Configuration/ZLinkSpotRegistrationValidator.cs:48`; C++ `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:4763`, `framework/languages/cpp/framework/src/runtime/mesh/mesh_node_runtime.cpp:4799`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/SpotNodeRegistration.java:114`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/SpotNodeRegistration.java:176`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkRouteMeshExtensions.kt:9`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkRouteMeshExtensions.kt:13`은 Java `ZLinkMeshNodeBuilder`를 그대로 반환하며 그 `setRoutingId`와 `objects`는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/configuration/ZLinkMeshNodeBuilder.java:19`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/configuration/ZLinkMeshNodeBuilder.java:43`에 있다; Kotlin DSL→전체 startup의 실행 결과는 미검증; Node `framework/languages/node/packages/framework/src/contracts/Configuration/RegistrationBuilders.ts:772`, `framework/languages/node/packages/framework/src/contracts/Configuration/RegistrationValidators.ts:291`.
- 확신: 높음 — 명시적 계약 충돌과 .NET validator의 잘못된 계약 출처는 확정적이다. 타 언어 최종 startup parity와 최종 정책 선택은 미확정이다.

### F-R8-13 C++ Spot timer에만 남은 raw monitoring 예외

- 분류: parity-gap
- 위치: 공통 금지는 `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:40`, `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:262`, `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:350`, `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:417`; C++ 자체 금지는 `framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.ko.md:320`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md:993`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md:295`; 반대 예외는 `framework/doc/framework/common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md:371`; 다른 언어는 `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-monitoring-errors.ko.md:92`, `framework/doc/framework/common/spec/server/languages/java/interfaces/monitoring.ko.md:286`, `framework/doc/framework/common/spec/server/languages/java/interfaces/configuration-host.ko.md:592`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.ko.md:47`, `framework/doc/framework/common/spec/server/languages/node/interfaces/03-location-observability.ko.md:152`, `framework/doc/framework/common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md:513`.
- 현재 규칙(인용): “C++는 Spot timer failure에 한해 public raw monitoring surface를 제공한다.”라고 하고 `add_spot_events(source_name)`·`on_spot_event(handler)`·`spot_event_t`를 예외로 둔다.
- 문제: 공통 계약이 금지한 raw DTO·event handler·source 등록을 C++ timer 실패에만 다시 공개한다. 실제 public header와 callback dispatch가 존재하므로 문서상의 오탈자가 아니다. 같은 실패를 기존 structured log와 별도 callback 두 경로로 관찰하게 한다. `**언어별 재량**` 표기나 관찰 결과가 같은 이유도 없으며 application이 수신하는 event 자체가 추가되므로 단순 언어 표현 차이로 볼 수 없다.
- 제안: Runtime monitoring §2·§9를 소유자로 삼아 “Spot timer 실패를 포함한 runtime 내부 오류는 표준 structured log로 관찰하며 별도 raw event DTO·handler·source 등록 표면을 제공하지 않는다.”로 통일한다.
- 규칙 수: before 2 → after 1 — structured log와 C++ timer 전용 raw callback이라는 두 공개 관찰 규칙을 하나로 줄인다.
- 행동 변경: 있음 — C++ application의 raw timer event 구독 API와 callback 수신이 사라진다.
- 영향: framework(cpp); .NET·Java·Kotlin·Node는 공개 status/표준 진단 투영을 비교했다. 다른 네 runtime의 모든 내부 timer 오류 처리 경로는 전수 검증하지 않았다.
- 성능 영향: 있음 — C++ timer 실패 경로에서 source 조회를 위한 state lane 진입, handler vector 복사, event 생성과 callback 순회가 제거될 수 있다. 일반 message hot path의 개선을 주장하지 않는다.
- 근거 코드: C++ `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/app.hpp:62`, `framework/languages/cpp/framework/include/zlink/framework/contracts/monitoring/spot_events.hpp:22`, `framework/languages/cpp/framework/src/runtime/diagnostics/monitoring_runtime.cpp:282`, `framework/languages/cpp/framework/src/runtime/diagnostics/monitoring_runtime.cpp:316`; .NET `framework/languages/dotnet/src/Zlink.Framework/Contracts/Configuration/ZLinkDrainContracts.cs:85`, `framework/languages/dotnet/src/Zlink.Framework/Contracts/Configuration/ZLinkDrainContracts.cs:165`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/monitoring/ZLinkObservedStatus.java:6`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:940`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkPublisherFlowBridge.kt:18`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkPublisherFlowBridge.kt:64`; Node `framework/languages/node/packages/framework/src/contracts/RouteMesh/Contracts.ts:127`, `framework/languages/node/packages/framework/src/contracts/RouteMesh/Contracts.ts:139`.
- 확신: 높음 — C++ 예외 표면과 실제 별도 callback 경로가 확인되며 공통 금지 문장과 직접 충돌한다.

### F-R8-14 Redis encoded blob에서 누락된 23 bytes

- 분류: parity-gap
- 위치: `framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md:121`, `framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md:122`, `framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md:135`, `framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md:292`; 올바른 투영은 `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/08-authority-relocation.ko.md:229`; 나머지는 `framework/doc/framework/common/spec/server/languages/cpp/interfaces/07-location-store.ko.md:260`, `framework/doc/framework/common/spec/server/languages/java/interfaces/location-maintenance.ko.md:238`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/location-maintenance.ko.md:25`, `framework/doc/framework/common/spec/server/languages/node/interfaces/08-location-maintenance.ko.md:219`.
- 현재 규칙(인용): 공통은 “공식 Redis provider의 최대 크기는 `64 MiB + 23 bytes`다.”라고 하는데 네 언어 투영은 “Blob 하나는 최대 64 MiB다.”라고 한다.
- 문제: Application data chunk 제한과 provider가 받는 encoded blob 제한이 섞였다. .NET provider는 envelope의 23 bytes를 포함한 상한을 쓰지만 C++·Java·Node는 provider 입력에서 64 MiB를 초과하면 거부한다. Kotlin 공식 provider는 Java 구현이다. 이 수치는 relocation adapter의 직접 handoff 크기 제한이 아니라 activation/recovery·terminal 기록용 blob의 provider 경계다.
- 제안: Relocation Store Redis §3의 크기 표를 단일 소유자로 삼아 “공식 Redis provider는 최대 64 MiB data chunk에 23-byte immutable envelope가 붙은 encoded blob, 즉 `64 MiB + 23 bytes`까지 수락하며 언어별 Store interface는 같은 입력 경계를 투영한다.”로 통일한다.
- 규칙 수: before 2 → after 1 — provider 입력 상한으로 잘못 사용한 64 MiB와 올바른 encoded 상한의 두 판정을 하나로 맞춘다; application data 제한은 그대로다.
- 행동 변경: 있음 — 길이가 `64 MiB + 1`부터 `64 MiB + 23`인 유효 provider 입력의 수락 여부가 달라진다.
- 영향: framework(cpp, java, kotlin, node); dotnet은 올바른 대조 구현이다. 최소 공개 API repro는 유효 reference·retention과 `64 MiB + 23` 길이 payload로 공식 provider의 `Put`을 호출하는 것이다. 이 repro는 실행하지 않았으며 아래 size validation의 정적 차이를 보고한다.
- 성능 영향: 없음 — copy·chunk 수·검증 횟수 최적화를 제안하지 않고 비교 상한의 의미를 맞춘다.
- 근거 코드: C++ `framework/languages/cpp/extensions/framework-locations-redis/include/zlink/locations/redis.hpp:965`, `framework/languages/cpp/extensions/framework-locations-redis/include/zlink/locations/redis.hpp:970`; .NET `framework/languages/dotnet/src/Zlink.Framework.Locations.Redis/ZLinkRedisRelocationStore.cs:18`, `framework/languages/dotnet/src/Zlink.Framework.Locations.Redis/ZLinkRedisRelocationStore.cs:104`, `framework/languages/dotnet/src/Zlink.Framework.Locations.Redis/ZLinkRedisRelocationStore.cs:339`; Java `framework/languages/java/zlink-framework-locations-redis/src/main/java/systems/zlink/framework/locations/redis/ZLinkRedisRelocationStore.java:28`, `framework/languages/java/zlink-framework-locations-redis/src/main/java/systems/zlink/framework/locations/redis/ZLinkRedisRelocationStore.java:72`, `framework/languages/java/zlink-framework-locations-redis/src/main/java/systems/zlink/framework/locations/redis/ZLinkRedisRelocationStore.java:177`; Kotlin은 같은 Java provider의 public `put`·validation을 사용하며 별도 Kotlin provider 구현은 확인되지 않았다; Node `framework/languages/node/packages/framework-locations-redis/src/relocation-store.ts:17`, `framework/languages/node/packages/framework-locations-redis/src/relocation-store.ts:32`, `framework/languages/node/packages/framework-locations-redis/src/relocation-store.ts:126`.
- 확신: 높음 — 네 runtime의 provider 입력 비교값과 Kotlin의 Java provider 재사용을 확인했다. Redis I/O는 실행하지 않았다.

### F-R8-15 Actor create 중복 옵션의 오류 분류 불일치

- 분류: parity-gap
- 위치: `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:411`; 언어 투영은 `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:253`, `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/06-actors.ko.md:312`, `framework/doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md:272`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.ko.md:20`, `framework/doc/framework/common/spec/server/languages/node/interfaces/05-actors.ko.md:134`. Framework 오류와 언어의 일반 인자 오류 구분은 `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:50`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.ko.md:81`이다.
- 현재 규칙(인용): “같은 option을 두 번 설정하면 `InvalidOperation`이다. Terminal submit을 두 번 실행하면 `InvalidOperation`이다.”
- 문제: Java Actor creation call의 중복 Mesh/request/timeout 및 재제출은 `IllegalStateException`으로 끝난다. Kotlin 옵션은 Java에 그대로 위임하므로 같은 예외가 노출되지만 terminal 재호출은 별도 Kotlin guard가 `INVALID_OPERATION`으로 가린다. Node는 중복 옵션에 internal `InvalidConfiguration`을 사용하고 매핑표가 이를 public `NotConfigured`로 바꾼다; Node의 재제출 오류는 `InvalidOperation`이 맞다. C++·.NET은 이 오류를 공통 `InvalidOperation`으로 분류한다. 타입 이름의 언어 관용 차이가 아니라 명시된 operation 오류가 다른 분류로 새는 결함이다.
- 제안: Actor 모델 §6.2를 소유자로 유지하여 “Actor create/get-or-create call의 중복 옵션과 재제출은 해당 call 소유자가 부작용 전에 공통 `InvalidOperation`으로 거부하며 언어 wrapper는 그 결과만 전달한다.”로 구현과 투영을 맞춘다.
- 규칙 수: before 3 → after 1 — 같은 중복 옵션에 대한 `InvalidOperation`, JVM `IllegalStateException`, `NotConfigured` 세 결과를 하나로 맞춘다.
- 행동 변경: 있음 — Java·Kotlin·Node application이 잡는 예외 타입 또는 `ErrorKind`가 달라진다.
- 영향: framework(java, kotlin, node); cpp·dotnet은 비교 기준이다. Node의 전체 internal error 매핑을 바꾸는 제안이 아니라 Actor create가 기존의 올바른 operation error를 사용하도록 하는 제안이다.
- 성능 영향: 없음 — 중복 옵션 검증이나 single-use guard 자체를 추가하지 않는다.
- 근거 코드: C++ `framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:103`, `framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:156`; .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:704`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:732`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:743`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2632`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2642`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2661`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:100`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:321`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:330`; Node `framework/languages/node/packages/framework/src/runtime/actors/index.ts:917`, `framework/languages/node/packages/framework/src/runtime/framework-errors-internal.ts:80`, `framework/languages/node/packages/framework/src/runtime/framework-errors-internal.ts:163`.
- 확신: 높음 — 공개 결과까지의 매핑을 확인했다. 최소 재현 입력은 같은 call의 `timeout` 또는 `inMesh`를 두 번 설정하는 것이며 실행하지 않았다.

### F-R8-16 Kotlin·Java가 같은 create 제출 여부를 이중 소유

- 분류: lower-layer-reverification
- 위치: 소유 규칙은 `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:396`, `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:411`; submission 전 Yield 거부는 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:55`; Java 투영은 `framework/doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md:272`, `framework/doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md:292`; wrapper 규칙은 `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.ko.md:20`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.ko.md:27`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.ko.md:234`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/README.ko.md:54`.
- 현재 규칙(인용): Kotlin은 “Kotlin 전용 single-use wrapper를 반환한다.”라고 하면서 다른 문맥의 Yield는 “reservation, factory 실행과 queue 변경 전에 `InvalidOperation`으로 끝낸다.”라고 한다.
- 문제: Kotlin `JavaActorCreateCall`·`JavaActorGetOrCreateCall`은 `KotlinSingleUse`의 AtomicBoolean을 먼저 바꾸고, 하위 Java call은 별도 AtomicBoolean으로 같은 제출 여부를 다시 판정한다. Java의 잘못된 재제출 예외(F-R8-15)를 wrapper가 가리는 효과도 있다. 특히 owner turn 밖의 Yield는 Java에서 제출 상태 변경 전에 거부되지만 Kotlin은 wrapper를 이미 소비한 뒤 Java의 거부를 받는다. 이후 같은 call을 정상 제출할 수 있는지가 달라지므로 wrapper guard를 ‘행동 변경 없음’으로 제거해서는 안 된다.
- 제안: Actor 모델 §6.2의 call 소유권 아래 “JVM Actor create/get-or-create의 제출 여부와 제출 전 거부는 Java call 하나가 소유하고 Kotlin wrapper는 suspend 완료 표현만 제공한다.”로 통일한다; F-R8-15의 Java 오류 정합을 선행 조건으로 둔다.
- 규칙 수: before 2 → after 1 — 동일 create call의 소비 상태와 atomic 판정 두 개를 하나로 줄인다. 다른 operation의 `KotlinSingleUse`까지 일괄 삭제하는 제안이 아니다.
- 행동 변경: 있음 — 잘못된 문맥의 Yield 이후 call 재사용 가능성과 재제출 예외가 달라진다.
- 영향: framework(kotlin, java) — Kotlin wrapper와 기존 Java ActorCreationCall 경계다; cpp·dotnet·node는 단일 제출 소유자의 비교 근거다.
- 성능 영향: 있음 — Kotlin Actor create마다 추가된 wrapper 소비 상태 allocation 및 terminal 호출의 추가 CAS 하나를 없앤다. 일반 message 송수신 전체의 개선 수치는 측정하지 않았다.
- 근거 코드: Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:100`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:317`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:330`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:341`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2615`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2662`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2676`; C++ `framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:95`, `framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:158`; .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:695`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:736`; Node `framework/languages/node/packages/framework/src/runtime/actors/index.ts:854`, `framework/languages/node/packages/framework/src/runtime/actors/index.ts:899`.
- 확신: 높음 — Kotlin과 Java의 guard 실행 순서가 명시적이다. 잘못된 Yield→정상 제출 시나리오는 실행하지 않았다.

### F-R8-17 C++ session bind에 따라붙은 무효 옵션과 Yield

- 분류: parity-gap
- 위치: `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:300`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:335`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:688`; 다른 투영은 `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md:92`, `framework/doc/framework/common/spec/server/languages/java/interfaces/stream-session.ko.md:72`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/stream-session.ko.md:77`, `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:376`; 공통 bind 완료는 `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:157`, `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:200`; Yield family는 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:55`, `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:62`.
- 현재 규칙(인용): C++ bind는 `request_call_t<session_actor_t> bind(actor_ref_t actor_ref);`를 반환하며 이 공통 request type에는 `timeout`, `metadata`, `async`, `yield`가 선언되어 있다.
- 문제: .NET·Java·Kotlin·Node는 bind 완료 자체를 비동기 값으로 투영하지만 C++는 일반 request builder를 재사용해 추가 조작 표면을 노출한다. 실제 bind와 bind-or-get의 submit lambda는 timeout·metadata 인자를 이름 없이 받고 사용하지 않는다. Yield는 generic call의 gate 변경까지 수행할 수 있지만 bind는 공통 허용 family에 없다. 언어 재량이라면 옵션 적용과 무시가 왜 같은 결과인지 설명해야 하는데 그런 근거가 없고 timeout 호출이 효과가 없는 public API가 된다.
- 제안: Session–Actor binding §5를 소유자로 삼아 “Session bind는 caller의 ActorRef에 대한 binding 완료를 비동기 값으로 반환하며 별도 request metadata·call timeout·Yield 조작을 제공하지 않는다.”로 다섯 투영을 맞추는 안을 감독에게 제시한다.
- 규칙 수: before 4 → after 1 — bind의 완료와 이에 따라붙은 metadata·timeout·Yield 규칙을 binding 완료 규칙 하나로 줄인다.
- 행동 변경: 있음 — C++ 반환형과 호출 가능한 fluent member가 달라진다. 새 bind 옵션을 공통 계약으로 정의할지는 별도 설계 결정이다.
- 영향: framework(cpp); dotnet·java·kotlin·node의 직접 비동기 bind를 대조했다. 다른 언어의 cancellation 수단까지 동일한 인자로 만들자는 제안은 아니다.
- 성능 영향: 없음 — 기존 bind의 transport·Store·timeout 내부 동작 최적화를 주장하지 않는다.
- 근거 코드: C++ `framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:948`, `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:69`, `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:83`, `framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:1633`, `framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:1662`; .NET `framework/languages/dotnet/src/Zlink.Framework/Contracts/Streams/IZLinkSession.cs:67`, `framework/languages/dotnet/src/Zlink.Framework/Contracts/Streams/IZLinkSession.cs:71`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActors.java:14`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActors.java:16`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:98`와 Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActors.java:16`; Node `framework/languages/node/packages/framework/src/contracts/Streams/IZLinkSessionActor.ts:6`, `framework/languages/node/packages/framework/src/contracts/Streams/IZLinkSessionActor.ts:7`.
- 확신: 높음 — C++에서 옵션 인자를 무시하는 코드와 다섯 public 반환형을 확인했다. Public API 변경 방향은 감독 승인이 필요하다.

### F-R8-18 Node HTTP typed status 실패의 잘못된 kind

- 분류: parity-gap
- 위치: `framework/doc/framework/common/spec/http-client/04-response-model.ko.md:13`, `framework/doc/framework/common/spec/http-client/09-error-model.ko.md:15`, `framework/doc/framework/common/spec/http-client/11-regression-tests.ko.md:29`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:156`; HTTP status 자동 retry 제외는 `framework/doc/framework/common/spec/http-client/06-redirect-retry-cookie.ko.md:30`이다.
- 현재 규칙(인용): “typed 제출의 status ≥ 400은 `InternalFailure`로 보고한다.”
- 문제: Node typed 실행은 status ≥ 400에서 `Unavailable`을 만든다. C++·.NET·Java는 `InternalFailure`이며 Kotlin은 Java decode 결과를 받는다. 같은 정상 HTTP 응답의 오류 분류가 전송 연결 실패 분류로 바뀌는 것이므로 application의 오류 처리가 달라진다. Retry 동작을 추가해서 보상할 문제가 아니다.
- 제안: HTTP 오류 모델 §9.1을 소유자로 유지하여 “Typed HTTP 제출이 수신한 status가 400 이상이면 모든 언어에서 `InternalFailure`로 완료하며 transport 연결 실패와 구분한다.”로 Node의 기존 분류를 맞춘다.
- 규칙 수: before 2 → after 1 — 같은 status 실패의 `InternalFailure`·`Unavailable` 두 판정을 하나로 맞춘다.
- 행동 변경: 있음 — Node application이 관찰하는 `ErrorKind`가 달라진다.
- 영향: framework(node); cpp·dotnet·java·kotlin은 동일 입력의 대조 구현이다. Raw response의 status를 오류로 바꾸는 제안이 아니다.
- 성능 영향: 없음 — 비교·decode·transport 경로는 그대로이며 분류값 정정이다.
- 근거 코드: Node `framework/languages/node/packages/http-client/src/request-builder.ts:216`, `framework/languages/node/packages/http-client/src/request-builder.ts:242`; C++ `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:184`, `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:205`; .NET `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:217`, `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:223`; Java `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:167`, `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:182`; Kotlin `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:40`, `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:49`와 Java decode.
- 확신: 높음 — status 조건과 public kind 생성이 직접 확인된다. HTTP endpoint를 호출하지 않았다.

### F-R8-19 C++ SessionActorManager의 별도 local create

- 분류: spec-impl-drift
- 위치: 공통 생성 소유자는 `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:394`, `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:477`, `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:484`; local-only binding 배제는 `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:98`; C++의 공개 Actor manager와 Session manager 선언은 `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:253`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:274`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:296`; 다른 SessionActors 투영은 `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md:89`, `framework/doc/framework/common/spec/server/languages/java/interfaces/stream-session.ko.md:68`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/stream-session.ko.md:10`, `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:374`.
- 현재 규칙(인용): 공통은 object-role Mesh의 “후보가 없다.”면 “`NotConfigured`로 끝난다.”라고 한다. C++ 문서의 `session_actor_manager_t`에는 bound/find/bind/bind-or-get만 있다.
- 문제: 실제 public header에는 별도 동기 `create`·`get_or_create` overload가 여전히 있다. 기본 Session manager 생성자는 자체 gateway state를 만들고, create dispatcher가 없으면 local node placeholder와 generation `1`의 ActorRef를 합성해 local map에 넣은 뒤 성공한다. 이는 공통 ActorManager의 Mesh 선택·authority 생성 완료와 별개인 두 번째 생성 소유자다. 단순히 문서에 누락 메서드를 추가하면 공통에 없는 local Actor 생성 계약을 승인하게 된다. 공개 API 최소 repro는 기본 생성한 `session_actor_manager_t`에서 `create("player", "p1")`를 호출하는 것이며 이 보고서는 실행 없이 해당 분기를 확인했다.
- 제안: Actor 모델 §6.2·§6.3을 생성의 단일 소유자로 삼아 “Actor 생성과 authority 확정은 공통 ActorManager의 create/get-or-create만 소유하며 SessionActorManager는 기존 global ActorRef의 binding만 제공한다.”로 두 번째 local create 표면과 fallback을 정리한다.
- 규칙 수: before 2 → after 1 — 공통 global Actor 생성과 Session manager의 placeholder 기반 local 생성이라는 두 소유자를 하나로 줄인다.
- 행동 변경: 있음 — 기존 C++ local create 호출의 공개 표면과 Store 없는 성공 결과가 달라진다.
- 영향: framework(cpp); dotnet·java·kotlin·node의 SessionActors 공개 표면에는 같은 local create 메서드가 없다. 이들 언어의 모든 내부 Actor 생성 경로를 전수 검증했다는 뜻은 아니다.
- 성능 영향: 없음 — 정상 global create hot path의 성능 변경을 주장하지 않는다. 우회 생성 경로의 제거가 제안의 목적이다.
- 근거 코드: C++ `framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:908`, `framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:928`, `framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:1399`, `framework/languages/cpp/framework/src/runtime/actors/actor_gateway_runtime.cpp:1474`, `framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:165`; .NET `framework/languages/dotnet/src/Zlink.Framework/Contracts/Streams/IZLinkSession.cs:63`, `framework/languages/dotnet/src/Zlink.Framework/Contracts/Streams/IZLinkSession.cs:75`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActors.java:9`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActors.java:18`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:98`와 Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActors.java:9`; Node `framework/languages/node/packages/framework/src/contracts/Streams/IZLinkSessionActor.ts:4`, `framework/languages/node/packages/framework/src/contracts/Streams/IZLinkSessionActor.ts:8`.
- 확신: 높음 — 공개 생성자에서 dispatcher 없는 placeholder 성공 분기까지 확인했다. 실행 repro는 하지 않았다.

### F-R8-20 Cold activation 순서의 다섯 언어 재서술

- 분류: form
- 위치: 소유 절은 `framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md:249`, `framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md:294`, `framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md:335`, `framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md:393`; 복제 절은 C++ `framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md:503`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md:520`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md:538`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md:570`, `framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md:591`; .NET `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.ko.md:604`, `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.ko.md:637`, `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.ko.md:667`; Java `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:264`, `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:301`, `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:359`; Kotlin `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:49`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:84`, `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:107`; Node `framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md:297`, `framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md:333`, `framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md:365`.
- 현재 규칙(인용): Kotlin은 “Target Java runtime은 metadata presence와 frame을 포함한 complete envelope를 Relocation Store에 immutable recovery root로 먼저 저장한다.”, “Source는 Ready 뒤 같은 message를 다시 보내지 않는다.”라고 순서를 재정의한다.
- 문제: Caller의 `instanceSpot(...)` marker와 타입 표현을 넘어 source·target·Store 사이의 저장 순서, reservation/CAS winner, barrier, first record, recovery pointer 제거를 다섯 언어가 다시 열거하고 sequence diagram까지 복제한다. 언어별 결과 차이나 재량이 아니라 같은 공통 protocol의 복사본이다. 본문 규칙·그림·검증 층에서 다시 말하는 부분도 있어 공통 변경을 여섯 문서에서 동시에 반영해야 한다.
- 제안: Spot address messaging §4를 유일한 소유자로 삼아 “`instanceSpot(...)`를 지정한 Missing Spot call은 공통 §4의 cold activation과 first-message 보존 계약을 따르며 언어별 문서는 marker·stable type·완료 타입만 투영한다.”로 연결한다.
- 규칙 수: before 6 → after 1 — 공통 한 문서와 다섯 언어 문서의 protocol 순서 소유자를 하나로 줄인다.
- 행동 변경: 없음 — marker, 실행 순서, durable 기록이나 복구 동작을 바꾸지 않는다.
- 영향: framework(cpp, dotnet, java, kotlin, node) — 공통 cold activation의 public 호출 투영과 문서 배치다.
- 성능 영향: 없음 — runtime의 CAS·copy·journal·queue를 제거하는 제안이 아니다.
- 근거 코드: C++ `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:680`, `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/channel.hpp:731`; .NET `framework/languages/dotnet/src/Zlink.Framework/Contracts/Spots/ZLinkSpot.cs:216`, `framework/languages/dotnet/src/Zlink.Framework/Contracts/Spots/ZLinkSpot.cs:227`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/spots/ZLinkSpotSendCall.java:7`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/spots/ZLinkSpotRequestCall.java:8`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/contracts/ZLinkOneWayContracts.kt:102`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/contracts/ZLinkOneWayContracts.kt:110`; Node `framework/languages/node/packages/framework/src/contracts/Spots/Contracts.ts:102`, `framework/languages/node/packages/framework/src/contracts/Spots/Contracts.ts:111`. 여기서는 public marker 표면을 대조했으며 다섯 runtime의 recovery 전이 전체는 전수 검증하지 않았다.
- 확신: 높음 — 동일한 공통 순서의 문서 복제는 명시적이다; implementation parity의 실행 증명으로 사용하면 안 된다.

## 언어별 행동 규칙 판정과 parity matrix

집계 단위는 아래에 열거한 **행동 규칙군**이다. 같은 의미를 여러 문장·예제·그림에서 다시 말한 경우 하나로 묶었다. 순수한 이름·generic·nullable·suspend/JVM 표기만의 차이는 행동 규칙 계수에서 제외하고 아래 API matrix에서 따로 다룬다. 같은 언어에서 공통 규칙과 반대 문장이 함께 있으면 해당 규칙군은 `c`로 센다. 따라서 이 수치는 원문 문장 수나 전체 public member 수의 통계가 아니다.

- `a`: 공통 소유자의 의미를 언어 문서가 재서술한다. 의미를 다시 정의하지 말고 필요한 결과·signature와 공통 링크를 남길 `form` 정리 대상이다.
- `b`: 구체 규칙이 언어 문서에만 있거나 공통 소유 범위가 비어 있다. 승격 후보 또는 언어 전용 표면의 계약 질문이며, 새 공통 동작을 이 보고서가 결정하지 않는다.
- `c`: 공통 규칙 또는 같은 규칙을 투영한 다른 문장과 충돌한다. `parity-gap`·`spec-impl-drift` 대상으로 분리했다.
- `—`: 해당 언어 문서에 독립적인 행동 재서술이 확인되지 않았거나 그 언어 전용 표면이 아니다. Java 타입을 그대로 쓰는 Kotlin 링크만으로 Kotlin에 규칙 복사본이 하나 더 있다고 세지 않았다.

구현 drift와 문장 분류는 별개다. 예를 들어 Actor create의 `InvalidOperation` 문장 자체는 다섯 언어 모두 공통 규칙과 일치하므로 `a`지만 Java·Kotlin·Node 구현은 F-R8-15의 결함을 가진다. HTTP one-way 역시 문서의 완료 구분과 구현의 실제 대기 경계를 혼동하면 안 된다.

| 언어 | a 공통 재서술 | b 공통 소유자 미확정 | c 충돌 | 판정한 규칙군 | — |
|---|---:|---:|---:|---:|---:|
| C++ | 32 | 3 | 6 | 41 | 3 |
| .NET | 35 | 2 | 3 | 40 | 4 |
| Java | 34 | 2 | 4 | 40 | 4 |
| Kotlin | 34 | 0 | 4 | 38 | 6 |
| Node | 33 | 2 | 4 | 39 | 5 |

아래 44개 규칙군을 기준으로 계산했다. P01–P31은 문서 중복의 분포를 보이는 목록이며 독립 finding 31개를 추가한 것이 아니다. 해당 공통 소유자는 실행 `01-execution`, 객체 `03-spot-actor`, session `04-session`, Store·host `05-location-relocation`, 관측 `06-observability`의 각 operation 계약이다. 구체 원인·수정 제안은 위 20개 finding으로 제한했다. HTTP 자체 전송 계약의 redirect·cookie·TLS·proxy·compression 규칙은 server 실행 규칙의 복제로 세지 않았다.

| 규칙군 | 판정 축 | C++ | .NET | Java | Kotlin | Node | 근거 구분 |
|---|---|---|---|---|---|---|---|
| P01 | One-way의 source-local admission 완료 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P02 | 일반 비동기 완료 동안 owner turn 유지 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P03 | Request Yield의 허용 문맥·제출 전 거부 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P04 | Worker call의 결과·완료·실행 문맥 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P05 | Actor Join의 intent·deferred barrier와 callback 완료 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P06 | Actor Join의 기본 timeout·입력 범위·deadline | a | a | a | a | a | 공통 소유 절의 재서술 |
| P07 | Actor create/get-or-create의 single-use·중복 옵션 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P08 | Actor create의 Mesh 선택·미구성·모호성 오류 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P09 | Actor 생성 결과·operation identity·terminal 기록 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P10 | Global Actor/Spot identity·ref snapshot·JSON generation | a | a | a | a | a | 공통 소유 절의 재서술 |
| P11 | Handler와 scoped dependency의 activation 수명 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P12 | Factory의 relocation policy 선택 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P13 | Capture/restore 소유권·실패·stale attempt 경계 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P14 | Instance cold activation의 first message·recovery 순서 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P15 | SpotWide/PerActor의 lane·seal·relocation 경계 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P16 | Timer overrun·tick·relocation 수명 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P17 | Instance Spot의 idle eviction·closing | a | a | a | a | a | 공통 소유 절의 재서술 |
| P18 | Session bind의 caller ActorRef·단일 route·hidden lookup 금지 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P19 | Rebind·logical disconnect·100 ms close 경계 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P20 | Relocation 뒤 bound ActorRef와 route snapshot 갱신 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P21 | STREAM의 framing·크기·codec·session 경계 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P22 | Host relocation mode·application version·target 선택 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P23 | Host relocation/shutdown의 deadline·공유 operation·waiter 취소 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P24 | Observation의 완전한 status·source별 합치기·구독별 loss | a | a | a | a | a | 공통 소유 절의 재서술 |
| P25 | Core HWM reserved status field의 고정 0 투영 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P26 | RouteMesh peer의 NotConnected/NotRequired 구분 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P27 | Fanout native-ready·첫 record/beacon·inbound liveness | a | a | a | a | — | 공통 소유 절의 재서술 |
| P28 | Placement weight·capacity와 공개 snapshot 경계 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P29 | Typed serializer 선택·canonical content type·fallback | a | a | a | a | a | 공통 소유 절의 재서술 |
| P30 | Location/Relocation Store의 opaque 값·원자성·수명 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P31 | Core byte HWM과 Framework job queue의 독립 소유권 | a | a | a | a | a | 공통 소유 절의 재서술 |
| P32 | command 44의 response·주기 재전송 | c | c | c | c | c | F-R8-4 |
| P33 | Automatic/object MeshNode에서 fixed RID 허용 | a | c | c | c | — | F-R8-12 |
| P34 | Redis provider encoded blob 상한 | c | a | c | c | c | F-R8-14 |
| P35 | 관측 loss counter의 수치 상한 | b | b | b | — | b | F-R8-1 |
| P36 | Raw monitoring DTO 금지와 C++ timer 예외 | c | a | a | a | a | F-R8-13 |
| P37 | C++ session bind의 metadata·timeout·Yield | b | — | — | — | — | F-R8-17 |
| P38 | Transport-facing STREAM bool Write | — | b | — | — | b | 추가 후보·BLOCKERS |
| P39 | Java public stopSpotRuntime | — | — | b | — | — | 추가 후보·BLOCKERS |
| P40 | C++ HTTP hosting·embedded server의 독립 동작 | b | — | — | — | — | 추가 후보·BLOCKERS |
| P41 | HTTP server Yield의 허용·금지 투영 | c | c | c | c | c | F-R8-8; 공통 언어 matrix도 포함 |
| P42 | HTTP decoded-body terminal의 비동기·blocking 의미 | c | a | a | a | a | F-R8-10 |
| P43 | HTTP timeout의 attempt·전체 operation 경계 | c | a | a | a | a | F-R8-9; 공통의 명시적 C++ 편차 포함 |
| P44 | NestJS HWM 설정의 반환 builder 소유권 | — | — | — | — | c | F-R8-6 |

P14의 전송·복구 순서는 F-R8-20에, P24·P25의 관측 투영은 F-R8-1·13에 대표 근거를 모았다. P27의 Node 칸을 `a`로 추정하지 않았다: Node 문서에서 내부 liveness topic 예약과 `NotRequired` 집계는 찾았지만 동일한 fanout 첫-record/beacon·15초 규칙 재서술은 확인하지 못했다. P35의 Kotlin도 Java 상한을 재사용할 뿐 별도 숫자 규칙을 복사하지 않아 `—`다. 이런 차이를 누락된 runtime 기능으로 판정하지 않았다.

| Operation family | C++ | .NET | Java | Kotlin | Node | 판정 |
|---|---|---|---|---|---|---|
| Send / request builders | one-way `async(): task_t<void>`; request `async`/`yield` | `Async(): ValueTask`; request `Async`/`Yield` | `submit(): CompletionStage<Void>`; request `submit`/`yield` | one-way `await(): Unit`; request `await`/`yield` suspend | `submit(): Promise<void>`; 허용 request에 `yield` | 완료 값·gate 의미를 보존하는 이름/awaitable 투영이다. Gate 이름 오류는 F-R8-3, HTTP 예외는 별도다. |
| Actor join | `timeout(milliseconds)`·`defer(): void` | `Timeout(TimeSpan)`·`Defer(): void` | `timeout(Duration)`·`defer(): void` | Java join call의 동기 `defer`; 완료 callback만 suspend bridge | `timeout(number)`·`defer(): void` | 어느 언어도 Join 자체에 async/Yield terminal을 추가하지 않는다. Deferred intent와 나중 completion이라는 관찰 결과가 같다. |
| Actor create/get-or-create | `in_mesh`·`creation_request`·`timeout`; `async`/`yield` | `InMesh`·`Request`·`Timeout`; `Async`/`Yield` | `inMesh`·`request`·`timeout`; `submit`/`yield` | 동일 옵션의 wrapper; `await`/`yield` | 동일 옵션; `submit`/`yield` | 문서의 옵션·result 축은 같다. 중복 옵션 오류와 Kotlin 이중 소비 상태는 F-R8-15·16, 문서 밖 C++ SessionActorManager local create는 F-R8-19이며 언어 관용으로 면제할 수 없다. |
| Session bind/bind-or-get | `request_call_t<session_actor_t>`; metadata·timeout·Yield까지 노출 | 직접 `ValueTask<IZLinkSessionActor>`와 CT | 직접 `CompletionStage<ZLinkSessionActor>` | Java bind API 재사용; `bindOrGetActor` suspend extension | 직접 `Promise<ZLinkSessionActor>`와 AbortSignal | C++의 추가 옵션은 F-R8-17이다. CT·AbortSignal·coroutine의 철자 차이만으로 동작 차이를 주장하지 않았으며 in-flight cancellation parity는 별도 미검증이다. |
| Host relocation/shutdown | `app_t::relocate`/`shutdown` task | `RelocateAsync`/`ShutdownAsync` ValueTask | `relocate`/`shutdown` CompletionStage; 별도 `stopSpotRuntime()` 노출 | Java host 그대로 사용; 별도 drain facade 없음 | `relocate`/`shutdown` Promise | Mode·options·result의 공통 소유자는 Host relocation이다. Java partial stop은 공통 계약 부재 후보이며 일반 shutdown의 다른 표기로 보지 않는다. |
| Monitoring | callback observation·status/loss 값; timer raw callback 예외 | `IAsyncEnumerable<ObservedStatus>` | `Flow.Publisher<ObservedStatus>` | 같은 Java 값의 Kotlin `Flow` bridge | `AsyncIterable<ObservedStatus>` | status/loss 값·포화 상한을 보존하는 소비 방식 차이는 타당하다. C++ timer event 추가는 별도 관찰 결과이므로 F-R8-13이다. |

이 matrix의 추가 public 구현 근거는 C++ `framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:451`, `framework/languages/cpp/framework/include/zlink/framework/contracts/actors/actor.hpp:483`, `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/app.hpp:92`; .NET `framework/languages/dotnet/src/Zlink.Framework/Contracts/Actors/IZLinkActorContext.cs:68`, `framework/languages/dotnet/src/Zlink.Framework/Contracts/Actors/IZLinkActorManager.cs:18`, `framework/languages/dotnet/src/Zlink.Framework/Contracts/Configuration/ZLinkDrainContracts.cs:178`; Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/actors/ZLinkActorJoinCall.java:5`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/actors/ZLinkActorCreateCall.java:7`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:972`, `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:1345`; Kotlin `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/contracts/ZLinkOneWayContracts.kt:86`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:98`, `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkPublisherFlowBridge.kt:18`; Node `framework/languages/node/packages/framework/src/contracts/Actors/ZLinkActorFactory.ts:30`, `framework/languages/node/packages/framework/src/contracts/Actors/ZLinkActorManager.ts:12`, `framework/languages/node/packages/framework/src/contracts/RouteMesh/RuntimeTopology.ts:150`이다. 각 언어의 구체 예외와 추가 표면은 해당 finding의 근거 코드를 함께 보아야 한다.

`doc/principal/documentation/spec-writing-guide.ko.md:399`는 방법의 차이를 `**언어별 재량**`으로 표시하고 동일 관찰 결과의 이유와 확인 기준을 함께 쓰도록 한다. 검토 범위의 `.ko.md`에서 이 표기는 **0건**이었다. 다만 단순한 표준 반환형·generic 표기가 다르다는 이유만으로 결함을 세지는 않았다. 다음 두 경우는 구분해야 한다.

- `uint64_t`·`ulong`·`long`·`bigint`라도 counter가 같은 `0..2^63-1` 값을 전달한다는 설명, 또는 Kotlin이 같은 Java status를 그대로 전달한다는 설명은 동일 관찰 결과의 근거가 있다. F-R8-1처럼 공통 수치 규칙을 먼저 모으고 구현 표현을 설명할 수 있다.
- Fixed RID 허용, HTTP 전체 deadline, HTTP Yield, C++ timer raw event는 구성 수락·완료 시각·gate·event 수신 자체를 바꾼다. 이들을 `언어별 재량`으로 표기하는 것만으로 parity가 성립하지 않는다. 확인 기준도 같은 입력의 public 결과여야 한다.

HTTP client는 독자적인 public exception 계층을 가진 별도 실행 framework가 아니다. HTTP 09는 Framework 공용 kind를 사용하며 HTTP가 자체 소유하는 것은 request·response·redirect·retry·cookie·TLS·proxy·compression의 전송 의미와 그 상황의 오류 매핑이다. Server turn과 gate는 공통 server 실행 계약을 따라야 한다. 현재 HTTP 05 §5.1·5.2와 12 §3은 같은 turn 유지·gate 반납 규칙을 다시 설명하고 충돌까지 만들었고, 09·12는 같은 오류 매핑 표를 중복 소유한다. 전송 계약 자체를 server 문서로 옮길 근거는 없다.

## 추가 후보(요약 1줄)

아래는 본문 20개에 포함하지 않은 후보다. `소유 범위 미확정`은 감독의 계약 결정 전에는 수정 제안으로 채택하지 않는다. 실행 결과를 확인하지 않은 언어를 추정해 확장하지 않았다.

- HTTP 오류 표 복사(`form`, 행동 변경: 없음): `framework/doc/framework/common/spec/http-client/09-error-model.ko.md:9`와 `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:150`이 같은 다섯 매핑을 소유하며 후자는 `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:158`에서 이미 전자를 소유자로 지정한다; 다섯 언어의 public kind 구현은 F-R8-18의 언어별 근거를 참조한다; **10개 매핑 문장 → 5개**, 초안 “HTTP의 상황별 오류와 cancellation은 09 오류 모델을 따른다.”를 12 §6에 두고 표는 09 §9.1에만 둔다.
- HTTP의 server gate 재서술(`form`, 행동 변경: 없음): `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:88`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:22`, `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:64`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:73`이 turn 유지·반납을 반복한다; execution adapter의 다섯 언어 코드 근거는 F-R8-8·11이다; **3개 문서 소유자 → 1개**, 초안 “HTTP operation의 turn 유지와 Worker Yield는 공통 Handler turn·execution gate §2·§3을 따르며 HTTP는 자신이 기다리는 전송·응답 경계만 정의한다.”를 HTTP 05 §5.2에 두고 gate 규칙은 server 공통 문서가 소유한다.
- C++ HTTP 검증 절의 내부 대기 조건(`form`, 행동 변경: 없음): `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:280`의 “내부 raw submit을 blocking wait로 기다리지 않는다”는 `doc/principal/documentation/spec-writing-guide.ko.md:707`의 공개 관찰 제한에 맞지 않는다; 구현 구분은 `framework/languages/cpp/http-client/src/client.cpp:563`, `framework/languages/cpp/http-client/src/client.cpp:573`이며 다른 언어 검증 절의 같은 문장 여부는 미확정이다; **2개 관찰 층 → 1개**, 초안 “`co_await` 제출은 호출 스레드의 다른 작업 진행을 막지 않고 typed response로 완료한다.”를 C++ HTTP §8에 두고 내부 raw 호출 형태를 검증 조건에서 삭제한다; public header compile 자체는 유효한 공개 signature 관찰이다.
- .NET·Node transport-facing STREAM bool write(소유 범위 미확정): 공통 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:35`의 직접 method naming 제외와 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:102`의 명시적 STREAM send 비동기 완료 범위를 먼저 구분해야 한다; 투영은 `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md:196`, `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md:208`, `framework/doc/framework/common/spec/server/languages/node/interfaces/06-stream-worker.ko.md:38`, 구현은 `framework/languages/dotnet/src/Zlink.Framework/Contracts/Streams/IZLinkStream.cs:16`, `framework/languages/node/packages/framework/src/contracts/Streams/IZLinkStream.ts:8`, `framework/languages/node/packages/framework/src/runtime/streams/managed-stream.ts:107`; 다른 세 언어의 동등한 raw transport surface 전체는 미검증이며 단순 명명 drift로 단정하거나 제거안을 확정하지 않는다.
- Java `stopSpotRuntime()`의 partial stop(소유 범위 미확정): `framework/doc/framework/common/spec/server/languages/java/interfaces/common-runtime.ko.md:105`와 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:881`·`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:885`는 boolean 반환 뒤 Spot close를 시작하지만 `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:761`의 host 전체 admission seal과 같은 operation인지 정하지 않는다; Kotlin은 Java runtime을 노출하고 다른 세 언어의 모든 partial-stop 표면은 미검증이므로 언어 전용 공통 계약 승격 여부를 감독에게 넘긴다.
- C++ HTTP hosting·embedded server의 계약 소유 범위(소유 범위 미확정): `framework/doc/framework/common/spec/server/languages/cpp/60-http-hosting.ko.md:9`, `framework/doc/framework/common/spec/server/languages/cpp/61-embedded-http-server.ko.md:9`, `framework/doc/framework/common/spec/server/languages/cpp/61-embedded-http-server.ko.md:29`가 자체 route·TLS·timeout·shutdown 계약을 두고 코드 `framework/languages/cpp/framework/include/zlink/framework/contracts/http/http.hpp:182`, `framework/languages/cpp/framework/include/zlink/framework/contracts/http/http.hpp:251`, `framework/languages/cpp/framework/include/zlink/framework/contracts/http/http.hpp:603`에 대응한다; `framework/doc/framework/common/spec/http-client/01-scope-and-architecture.ko.md:7`의 client 계약과 같은 대상이 아니며 다른 언어에 동일 내장 server를 요구할 근거는 확인하지 못했으므로 공통 승격 범위를 결정하지 않는다.
- Java·Kotlin HTTP body 크기 초과 kind(`parity-gap`, 행동 변경: 있음): `framework/doc/framework/common/spec/http-client/09-error-model.ko.md:13`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:154`는 `CapacityExceeded`인데 `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/internal/ResponseBodyReader.java:63`, `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/internal/ResponseCompression.java:54`는 `INTERNAL_FAILURE`를 만들며 Kotlin은 `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:40`·`framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:49`로 같은 Java 요청을 기다린다; 다른 세 언어의 이 분기 전체는 미검증; **2개 분류 → 1개**, 초안 “설정한 response body byte 제한 초과는 `CapacityExceeded`다.”의 소유자는 HTTP 09 §9.1이다.
- Java·Kotlin HTTP redirect 형식 오류 kind(`parity-gap`, 행동 변경: 있음): `framework/doc/framework/common/spec/http-client/09-error-model.ko.md:11`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:152`는 `ProtocolError`인데 `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/internal/RedirectPolicy.java:61`·`framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/internal/RedirectPolicy.java:72`의 지원하지 않는 Location 형식은 `INTERNAL_FAILURE`다; Kotlin 전달 경로는 앞 후보의 두 bridge 위치와 같고 다른 세 언어의 이 분기는 미검증; **2개 분류 → 1개**, 초안 “지원 계약에 맞지 않는 redirect Location 형식은 `ProtocolError`다.”의 소유자는 HTTP 09 §9.1이며 redirect 지원 범위를 넓히는 제안은 아니다.
- Node HTTP one-shot 재제출 kind(`parity-gap`, 행동 변경: 있음): `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:114`·`framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:116`은 `InvalidOperation`을 요구하지만 `framework/languages/node/packages/http-client/src/request-builder.ts:69`·`framework/languages/node/packages/http-client/src/request-builder.ts:72`는 `ProtocolError`이고 .NET `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:141`·`framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:145`는 계약과 같다; C++·Java·Kotlin의 이 분기는 미검증; **2개 분류 → 1개**, 초안 “이미 제출한 one-shot HTTP 요청의 재제출은 `InvalidOperation`이다.”의 소유자는 HTTP 05 §5.5다.
- C++ HTTP terminal의 이전 명칭(`parity-gap`, 행동 변경: 있음): 공통 Submit naming은 HTTP를 포함하는 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:31`과 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:50`에서 C++ `async`로 정하지만 `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:14`·`framework/doc/framework/common/spec/http-client/05-execution-model.ko.md:40`, `framework/doc/framework/common/spec/http-client/12-http-client.ko.md:65`, `framework/doc/framework/common/spec/http-client/language-interfaces.ko.md:58`, `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md:65` 및 `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:332`는 `submit`을 유지한다; 다른 언어 변경 제안은 없고 F-R8-3의 server gate 검사 대상과도 별개다; **2개 명명 규칙 → 1개**, 초안 “HTTP를 포함한 C++ messaging builder의 정상 비동기 terminal 이름은 `async`다.”의 소유자는 공통 Submit §2이며 공개 C++ API 변경이므로 즉시 문서 정정으로 처리하지 않는다.
- Java·Kotlin local Actor bind overload(`spec-impl-drift`, 행동 변경: 있음): Kotlin `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/stream-session.ko.md:10`은 local Actor overload가 없다고 하고 Java `framework/doc/framework/common/spec/server/languages/java/interfaces/stream-session.ko.md:68`은 ActorRef bind만 투영하지만 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActors.java:12`·`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/streams/ZLinkSessionActors.java:14`에 두 overload가 있으며 Kotlin은 `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:98`과 같은 Java session 표면을 쓴다; **2개 bind 입력 표면 → 1개**, 초안 “Session bind는 caller가 지정한 global ActorRef를 입력으로 받는다.”의 소유자는 `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:98`이며 overload 제거는 공개 API 변경이다; 내부에서 local Actor를 global ref로 바꾸는 전체 경로와 다른 세 언어의 유사 overload는 미검증이므로 authority 우회까지 주장하지 않는다.

## 읽은 범위

R8 지정 범위의 한국어 문서 **86개, 20,629행을 모두 읽었다**. 아래 행 수는 공백·코드 블록·표·그림 source를 포함한 파일의 물리 행 수이며 각 파일의 실제 읽은 범위는 `1–마지막 행`이다. 같은 파일을 재확인한 행은 중복 합산하지 않았다. 출력이 잘렸던 .NET common runtime·topology 구간은 별도 구간 읽기로 보완했다. 지정한 한국어 문서 중 생략한 파일은 없다.

| 범위 | 파일 수 | 읽은 행 수 |
|---|---:|---:|
| 언어 공통 목차 | 1 | 32 |
| cpp | 13 | 6,297 |
| dotnet | 16 | 3,931 |
| java | 14 | 3,499 |
| kotlin | 12 | 1,784 |
| node | 11 | 3,433 |
| HTTP client | 19 | 1,653 |

| 파일 | 읽은 행 수 | 읽은 범위 |
|---|---:|---|
| `framework/doc/framework/common/spec/server/languages/README.ko.md` | 32 | 1–32 |
| `framework/doc/framework/common/spec/server/languages/cpp/01-system-structure.ko.md` | 274 | 1–274 |
| `framework/doc/framework/common/spec/server/languages/cpp/60-http-hosting.ko.md` | 610 | 1–610 |
| `framework/doc/framework/common/spec/server/languages/cpp/61-embedded-http-server.ko.md` | 363 | 1–363 |
| `framework/doc/framework/common/spec/server/languages/cpp/README.ko.md` | 40 | 1–40 |
| `framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.ko.md` | 382 | 1–382 |
| `framework/doc/framework/common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md` | 1,118 | 1–1118 |
| `framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md` | 1,116 | 1–1116 |
| `framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md` | 921 | 1–921 |
| `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md` | 338 | 1–338 |
| `framework/doc/framework/common/spec/server/languages/cpp/interfaces/06-stream-session.ko.md` | 215 | 1–215 |
| `framework/doc/framework/common/spec/server/languages/cpp/interfaces/07-location-store.ko.md` | 490 | 1–490 |
| `framework/doc/framework/common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md` | 389 | 1–389 |
| `framework/doc/framework/common/spec/server/languages/cpp/interfaces/README.ko.md` | 41 | 1–41 |
| `framework/doc/framework/common/spec/server/languages/dotnet/README.ko.md` | 64 | 1–64 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/01-common-runtime.ko.md` | 238 | 1–238 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md` | 157 | 1–157 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/03-configuration-topology.ko.md` | 697 | 1–697 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/04-channel-messaging.ko.md` | 156 | 1–156 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.ko.md` | 683 | 1–683 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/06-actors.ko.md` | 340 | 1–340 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-bound-stream-session.ko.md` | 26 | 1–26 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md` | 209 | 1–209 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/08-authority-relocation.ko.md` | 253 | 1–253 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md` | 229 | 1–229 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/08-location-provider-redis.ko.md` | 109 | 1–109 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-monitoring-errors.ko.md` | 98 | 1–98 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md` | 508 | 1–508 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/11-serialization.ko.md` | 120 | 1–120 |
| `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/README.ko.md` | 44 | 1–44 |
| `framework/doc/framework/common/spec/server/languages/java/01-system-structure.ko.md` | 22 | 1–22 |
| `framework/doc/framework/common/spec/server/languages/java/02-handler-interfaces.ko.md` | 20 | 1–20 |
| `framework/doc/framework/common/spec/server/languages/java/03-location-store.ko.md` | 18 | 1–18 |
| `framework/doc/framework/common/spec/server/languages/java/README.ko.md` | 40 | 1–40 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/README.ko.md` | 53 | 1–53 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md` | 295 | 1–295 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/channel-messaging.ko.md` | 317 | 1–317 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/common-runtime.ko.md` | 391 | 1–391 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/configuration-host.ko.md` | 782 | 1–782 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/location-maintenance.ko.md` | 341 | 1–341 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/location-objects.ko.md` | 39 | 1–39 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/monitoring.ko.md` | 287 | 1–287 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md` | 708 | 1–708 |
| `framework/doc/framework/common/spec/server/languages/java/interfaces/stream-session.ko.md` | 186 | 1–186 |
| `framework/doc/framework/common/spec/server/languages/kotlin/02-handler-interfaces.ko.md` | 20 | 1–20 |
| `framework/doc/framework/common/spec/server/languages/kotlin/03-location-store.ko.md` | 18 | 1–18 |
| `framework/doc/framework/common/spec/server/languages/kotlin/README.ko.md` | 52 | 1–52 |
| `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/README.ko.md` | 81 | 1–81 |
| `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.ko.md` | 253 | 1–253 |
| `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/channel-messaging.ko.md` | 271 | 1–271 |
| `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/common-runtime.ko.md` | 90 | 1–90 |
| `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/configuration-host.ko.md` | 239 | 1–239 |
| `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/location-maintenance.ko.md` | 87 | 1–87 |
| `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.ko.md` | 88 | 1–88 |
| `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md` | 426 | 1–426 |
| `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/stream-session.ko.md` | 159 | 1–159 |
| `framework/doc/framework/common/spec/server/languages/node/01-system-structure.ko.md` | 333 | 1–333 |
| `framework/doc/framework/common/spec/server/languages/node/README.ko.md` | 30 | 1–30 |
| `framework/doc/framework/common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md` | 523 | 1–523 |
| `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md` | 468 | 1–468 |
| `framework/doc/framework/common/spec/server/languages/node/interfaces/03-location-observability.ko.md` | 549 | 1–549 |
| `framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md` | 393 | 1–393 |
| `framework/doc/framework/common/spec/server/languages/node/interfaces/05-actors.ko.md` | 195 | 1–195 |
| `framework/doc/framework/common/spec/server/languages/node/interfaces/06-stream-worker.ko.md` | 173 | 1–173 |
| `framework/doc/framework/common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md` | 447 | 1–447 |
| `framework/doc/framework/common/spec/server/languages/node/interfaces/08-location-maintenance.ko.md` | 283 | 1–283 |
| `framework/doc/framework/common/spec/server/languages/node/interfaces/README.ko.md` | 39 | 1–39 |
| `framework/doc/framework/common/spec/http-client/01-scope-and-architecture.ko.md` | 52 | 1–52 |
| `framework/doc/framework/common/spec/http-client/02-client-builder.ko.md` | 55 | 1–55 |
| `framework/doc/framework/common/spec/http-client/03-request-builder.ko.md` | 63 | 1–63 |
| `framework/doc/framework/common/spec/http-client/04-response-model.ko.md` | 48 | 1–48 |
| `framework/doc/framework/common/spec/http-client/05-execution-model.ko.md` | 123 | 1–123 |
| `framework/doc/framework/common/spec/http-client/06-redirect-retry-cookie.ko.md` | 63 | 1–63 |
| `framework/doc/framework/common/spec/http-client/07-auth-tls-proxy.ko.md` | 33 | 1–33 |
| `framework/doc/framework/common/spec/http-client/08-compression.ko.md` | 19 | 1–19 |
| `framework/doc/framework/common/spec/http-client/09-error-model.ko.md` | 39 | 1–39 |
| `framework/doc/framework/common/spec/http-client/10-revision-candidates.ko.md` | 25 | 1–25 |
| `framework/doc/framework/common/spec/http-client/11-regression-tests.ko.md` | 63 | 1–63 |
| `framework/doc/framework/common/spec/http-client/12-http-client.ko.md` | 170 | 1–170 |
| `framework/doc/framework/common/spec/http-client/README.ko.md` | 58 | 1–58 |
| `framework/doc/framework/common/spec/http-client/language-interfaces.ko.md` | 117 | 1–117 |
| `framework/doc/framework/common/spec/http-client/languages/cpp/cpp-http-client.ko.md` | 339 | 1–339 |
| `framework/doc/framework/common/spec/http-client/languages/dotnet/dotnet-http-client.ko.md` | 122 | 1–122 |
| `framework/doc/framework/common/spec/http-client/languages/java/java-http-client.ko.md` | 94 | 1–94 |
| `framework/doc/framework/common/spec/http-client/languages/kotlin/kotlin-http-client.ko.md` | 85 | 1–85 |
| `framework/doc/framework/common/spec/http-client/languages/node/node-http-client.ko.md` | 85 | 1–85 |

기준 자료는 root `AGENTS.md`와 `doc/plan/c016-worklog/spec-review/README.ko.md`를 먼저 읽고, `doc/AGENTS.md`, `framework/AGENTS.md`, `framework/doc/AGENTS.md`, `framework/languages/dotnet/AGENTS.md`의 적용 범위를 확인했다. `doc/principal/documentation/spec-writing-guide.ko.md`의 §1·§2.4·§4.2–4.4·§9.3, `doc/principal/documentation/documentation-principles.ko.md`의 관련 한국어 설명 원칙, `doc/plan/c016-worklog/decisions.ko.md` D-090–D-101을 기준으로 삼았다. 기준 문서·공통 server spec 전체의 정독을 주장하지 않으며 위 20,629행에 합산하지 않았다.

공통 server 문서는 finding에 인용한 operation 소유 절과 주변 문장만 대조했다. 주요 범위는 Submit/Execution gate, MeshNode·identity, Actor create·join, Spot address messaging·routing, Session binding·wire command 44, Redis encoded blob, Host relocation, Runtime monitoring이다. 구현은 각 finding의 언어별 public 선언과 해당 분류·소비·완료 분기를 읽었다. `framework/languages/kotlin/`은 별도 구현 디렉터리가 아니며 Kotlin 코드는 `framework/languages/java/zlink-framework-kotlin/`과 `framework/languages/java/zlink-http-client-kotlin/`에서 확인했다. Java 재사용을 코드로 확인한 경우만 Kotlin에도 적용했다. 구현 파일 전체·모든 overload·startup·race·recovery 전이 전체를 읽거나 실행했다는 뜻은 아니며 미확인 경계는 각 finding에 적었다.

읽지 않은 범위는 지정 밖의 `.en.md`, `core/**`·`bindings/**` 구현, 다른 job 소유의 전체 공통 spec, 전체 test·sample·benchmark다. 예외적으로 알려진 gate drift를 확인하려고 `scripts/verify-framework-submit-api.sh`의 contract 검사 구간과 `cfa70e8a681b7f6d653329e1c1aef244298e3a46` 변경 이력을 읽었다. 이 script나 test를 실행하지 않았다. 별도 ledger·계획서·repro source·임시 파일은 만들지 않았다.

## BLOCKERS

- F-R8-11: HTTP one-way의 “전송 경계에 제출” 완료를 어떤 공개 관찰로 확정할 것인가—transport가 요청을 수락한 시점과 실제 request bytes 송신 완료 중 어느 것이 계약이며, response 수신 전 실패는 어느 공개 경로로 전달해야 하는가?
- F-R8-8·9·10: HTTP 12의 HTTP Yield 금지·blocking terminal 금지와 HTTP 06의 시도당 timeout을 공통 정본으로 확정할 것인가, 아니면 기존 C++ 전체 deadline 및 다섯 언어의 server HTTP Yield를 별도 설계 변경으로 채택할 것인가?
- F-R8-12: Fixed RID는 공통 MeshNode의 manual·non-object 제한으로 통일할 것인가, 현재 .NET·Java·Kotlin 문서의 automatic/object 허용을 새 공통 계약으로 채택할 것인가?
- F-R8-17: Session bind는 기존 네 언어처럼 직접 비동기 ActorRef binding으로 한정할 것인가, C++ request builder의 timeout·metadata·Yield를 실제 의미를 가진 공통 옵션으로 승격할 것인가?
- 추가 후보의 .NET·Node `IZLinkStream.Write`/`ZLinkStream.write`는 공통 Submit §4의 명시적 STREAM send에 포함되는가, 아니면 typed session call과 구분되는 transport-facing 계약이며 그 소유 절은 어디인가?
- Java `stopSpotRuntime()`와 C++ HTTP hosting/embedded server는 의도적으로 언어에 한정된 제품 표면인가, 공통 기능의 누락된 투영인가; 전자라면 각 표면의 적용 범위와 공통 lifecycle과의 경계를 어느 문서가 소유해야 하는가?
