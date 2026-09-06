# R4 — Foundation·Execution 스펙 심층 리뷰

읽기 전용 진단이며 구현·스펙 수정, build·test·benchmark 실행은 하지 않았다. Findings 19개 중 문서 통합·정정 17개는 `행동 변경: 없음`, 구현의 공개 결과가 달라지는 F-R4-5·6은 `행동 변경: 있음`으로 분류했다. 후자의 적용 시점은 사용자 지시대로 0.18.0이다.

R4 본문 17개 파일 8,773행을 모두 읽었다. 검토 시작 HEAD는 `90f6e14f8b4f646b11f1a0db6e921fdf51e302ee`, 인용 재확인 HEAD는 `7627284944fe2df46f7f7a3b3a00795b22d5b927`이다. 그 사이 다른 작업이 바꾼 인접 문서는 변경 구간과 링크 집계를 다시 확인했다. R4 본문과 여기서 대조한 Framework runtime 코드는 두 HEAD 사이에서 동일하다. 아래 행 번호는 재확인 시점 기준이다.

규칙 수는 finding별로 충돌·중복하는 판정의 정의 위치 또는 구현 정책을 센다. 관찰 요구와 참조 링크를 별도 정책으로 부풀리지 않았으며, finding 사이의 관련 인용이 겹치므로 열 전체를 합산하지 않는다.

| 번호 | 제목 | 분류 | 행동 변경 | 규칙 수 | 성능 영향 | 확신 |
|---|---|---|---|---|---|---|
| F-R4-1 | Binding cancellation 소유권의 중복 서술 | `lower-layer-reverification` | 없음 | 6 → 1 | 없음 | 높음 |
| F-R4-2 | Admission seal 수락 조건의 소유권 분산 | `scattered-control` | 없음 | 5 → 1 | 없음 | 높음 |
| F-R4-3 | Local·remote queue 오류 매핑의 중복 | `consolidation` | 없음 | 5 → 1 | 없음 | 높음 |
| F-R4-4 | Yield 제공 call 목록의 중복 | `consolidation` | 없음 | 3 → 1 | 없음 | 높음 |
| F-R4-5 | Node completion의 process 공유 예약 누락 | `parity-gap` | 있음 | 2 → 1 | 있음 | 높음 |
| F-R4-6 | 허용되지 않은 Yield의 언어별 오류 차이 | `parity-gap` | 있음 | 3 → 1 | 없음 | 높음 |
| F-R4-7 | Payload·relocation의 retained Core credit lease 잔재 | `spec-impl-drift` | 없음 | 5 → 1 | 없음 | 높음 |
| F-R4-8 | Framework service control의 물리 lane 배정 불일치 | `spec-impl-drift` | 없음 | 4 → 1 | 없음 | 높음 |
| F-R4-9 | Execution domain과 owner FIFO의 혼동 | `spec-impl-drift` | 없음 | 2 → 1 | 없음 | 높음 |
| F-R4-10 | Lifecycle generation의 수명 정의 불일치 | `spec-impl-drift` | 없음 | 2 → 1 | 없음 | 높음 |
| F-R4-11 | State lane 재진입 검사의 release 예외 | `spec-impl-drift` | 없음 | 2 → 1 | 없음 | 높음 |
| F-R4-12 | 용어집 항목의 spec 링크 미참조 | `form` | 없음 | 132 → 132 | 없음 | 높음 |
| F-R4-13 | 공통 실행 용어의 용어집 미등록 | `form` | 없음 | 9 → 9 | 없음 | 높음 |
| F-R4-14 | 실행 queue owner와 MeshNode Owner의 혼동 | `form` | 없음 | 2 → 1 | 없음 | 높음 |
| F-R4-15 | Backpressure 정의의 remote receive-flow 누락 | `form` | 없음 | 2 → 1 | 없음 | 높음 |
| F-R4-16 | Reply token 정의의 STREAM 적용 범위 | `form` | 없음 | 2 → 1 | 없음 | 중간 |
| F-R4-17 | Spot application queue의 control 분류 불일치 | `form` | 없음 | 3 → 1 | 없음 | 높음 |
| F-R4-18 | Snapshot 정의의 anchor 충돌 | `form` | 없음 | 2 → 1 | 없음 | 높음 |
| F-R4-19 | Payload 검증 요구의 white-box 조건 중복 | `form` | 없음 | 16 → 8 | 없음 | 높음 |

### F-R4-1 Binding cancellation 소유권의 중복 서술

- 분류: lower-layer-reverification
- 위치: `bindings/doc/spec/async-execution-model.ko.md:108`; `bindings/doc/spec/async-coroutine-policy.ko.md:64`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:147`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:238`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:265`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:566`; `framework/doc/framework/common/spec/server/01-execution/03-cancellation-and-shutdown.ko.md:22`; `framework/doc/framework/common/spec/server/01-execution/03-cancellation-and-shutdown.ko.md:69`; `framework/doc/framework/common/spec/server/01-execution/03-cancellation-and-shutdown.ko.md:186`
- 현재 규칙(인용): “Caller가 기다림을 중단한 뒤 도착한 completion은 socket owner가 drain하고 native payload를 정리한다.”
- 문제: Native operation을 기다리던 caller의 취소, late completion 정리, 두 번째 terminal 금지는 binding의 책임인데 submit 문서 세 절과 cancellation 문서 두 절이 같은 소유권 전이를 다시 정의한다. Framework가 자신의 admission queue에서 work를 제거하는 규칙과 service-wire OperationId의 terminal 경쟁은 별개의 상위 책임이다. 이 둘까지 native registry와 합치면 안 된다. 확인한 코드는 의미가 다른 두 registry를 사용하며, 이 finding은 추가 poller가 존재한다는 주장이 아니라 하위 계약의 문서 중복 진단이다.
- 제안: 단일 소유자는 bindings/doc/spec/async-execution-model.ko.md §6이고 Framework cancellation §3은 경계만 참조한다: “Binding operation의 caller-wait cancellation과 late native completion 정리는 binding의 비동기 실행 모델을 따르며, Framework는 binding에 넘기기 전 자신이 소유한 queue 대기의 취소를 담당한다.”
- 규칙 수: before 6 → after 1 — Binding 원본 1곳과 Framework의 독립 설명 5곳을 센다(검증용 관찰 문장은 같은 규칙의 투영으로 중복 계수하지 않음).
- 행동 변경: 없음 — 제안은 하위 계약으로의 링크 통합이며 취소 결과·전송 가능성·정리 순서를 바꾸지 않는다.
- 영향: bindings(.NET), framework(.NET); C++·Java·Kotlin·Node의 native cancellation cleanup 자체는 이 finding에서 검증하지 않았다. 대표 구현 위치: `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:1501`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 문서 통합이다. Native registry나 Framework service completion registry를 삭제하는 제안이 아니다.
- 근거 코드: `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:1501` — caller 취소와 WaitingWritable payload 정리; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:12278` — binding Async(cancellationToken) 소비; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:150` — 별도 Framework service operation의 terminal 경쟁
- 확신: 높음 — 하위 소유권 조항과 반복 문장은 직접 확인했다. 전체 언어의 native cleanup 적합성 판정으로 확대하지 않았다.

### F-R4-2 Admission seal 수락 조건의 소유권 분산

- 분류: scattered-control
- 위치: `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:759`; `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:423`; `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:562`; `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:616`; `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md:149`; `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md:273`; `framework/doc/framework/common/spec/server/01-execution/03-cancellation-and-shutdown.ko.md:125`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:161`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:413`; `doc/plan/c016-worklog/decisions.ko.md:1310`; `doc/plan/c016-worklog/decisions.ko.md:1347`
- 현재 규칙(인용): “같은 seal을 mesh node도 따른다” / “`Relocating`·`Draining` 중에는 이미 admission을 마친 message와 진행 중인 completion만”
- 문제: D-097/D-098과 host relocation §14는 host shutdown seal 하나를 mesh가 조회하도록 이미 결정했다. 그러나 interaction §11·검증, cancellation §5, layering §4, mesh §8은 각각 수락 중단 시점과 drain 순서를 규정한다. 특히 interaction 검증은 Relocating에서도 신규 existing-owner message를 막는 것으로 읽혀, 같은 문서 565행의 unit permit 전 계속 처리와 충돌한다. Execution gate는 수락한 handler의 직렬 실행 권한이고 shared permit은 용량 권한이므로 host/unit admission seal과 같은 판정이 아니다.
- 제안: 단일 소유자는 05-host-relocation-flow.ko.md §14 및 그 문서의 검증 요구다: “신규 작업 수락은 Shutdown의 host admission seal 또는 Relocate의 해당 unit seal이 결정하고 mesh는 host shutdown seal을 조회하며, execution gate와 queue permit은 그 판정을 전제로 실행 순서와 용량만 관리한다.”
- 규칙 수: before 5 → after 1 — 수락 중단·drain 단계의 독립 규정 위치 5곳 → host lifecycle 계약 1곳; 서로 다른 host seal·unit seal·execution gate·permit을 하나의 상태로 합치는 계수가 아니다.
- 행동 변경: 없음 — 최신 D-097/D-098 계약을 재참조하는 문서 통합이며 runtime의 수락 조건을 바꾸지 않는다.
- 영향: framework(C++, .NET, Java, Kotlin, Node). 아래 코드의 기존 admission owner를 유지한다; Node mesh의 Hello 차단까지 충족하는지는 미검증이다. 대표 구현 위치: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkDrainAdmissionGate.cs:99`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 새 flag·gate·조회 경로를 만들거나 기존 실행 gate를 없애지 않는다.
- 근거 코드: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkDrainAdmissionGate.cs:99` / `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkDrainAdmissionGate.cs:118` — shutdown 소유 seal; `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:2764` / `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3911` — host seal 조회; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:316` / `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/drain/ZLinkMeshDrainCoordinator.java:66` — 기존 coordinator 조회; `framework/languages/node/packages/framework/src/runtime/admission.ts:33` / `framework/languages/node/packages/framework/src/runtime/host/route-mesh-runtime.ts:464` — Node admission과 drain; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:33` — Kotlin은 JVM runtime에 호출 위임; 독립 host seal 구현은 확인하지 않음
- 확신: 높음 — 문서 소유권 분산과 Relocating 검증 문장의 충돌은 확정했다. D-097 전체 언어 회귀 검증을 수행한 것은 아니다.

### F-R4-3 Local·remote queue 오류 매핑의 중복

- 분류: consolidation
- 위치: `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:90`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:275`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:287`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:304`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:415`; `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md:735`; `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md:758`; `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md:770`
- 현재 규칙(인용): “같은 runtime의 Spot·Actor 대기열이 가득 차면 즉시 `CapacityExceeded`, 다른 node의 대기열이면 `Unavailable`로 끝낸다.”
- 문제: Error model §5가 실패 자원의 local/remote 소유권과 placement 예외를 정의하지만, execution gate는 Spot messaging §5.3이 판정을 소유한다고 선언하고 다시 표를 싣는다. Backpressure와 Spot messaging도 같은 매핑을 반복한다. 현재 네 runtime의 remote workerQueueFull 매핑은 Unavailable로 일치한다. 원인은 구현 불일치가 아니라 오류 종류를 고르는 계약의 소유자가 둘 이상이라는 점이다.
- 제안: 단일 소유자는 07-framework-error-model.ko.md §5 및 §9의 Request 오류 관찰이다: “Request의 bounded queue 확보 실패는 source runtime이 소유한 자원이면 CapacityExceeded, 다른 node가 소유한 자원이면 Unavailable로 완료하며, placement capacity는 이 queue 판정에서 제외한다.”
- 규칙 수: before 5 → after 1 — Error model 1곳, execution 표 1곳, backpressure 1곳, Spot messaging 표·control 재서술 2곳 → 매핑 1곳.
- 행동 변경: 없음 — 현재 구현의 local/remote 오류 선택을 유지하고 다른 문서의 결과 칸을 단일 매핑의 링크로 바꾼다.
- 영향: framework(C++, .NET, Java, Kotlin, Node). Queue admission 자체를 변경하지 않는다. 대표 구현 위치: `framework/languages/cpp/framework/src/runtime/messaging/request_failure_mapper.cpp:232`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 문서 통합으로 hot path의 매핑·lock·예약 수는 그대로다.
- 근거 코드: `framework/languages/cpp/framework/src/runtime/messaging/request_failure_mapper.cpp:232`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkRequestFailureMapper.cs:65`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/backend/ZLinkBackendRequestResult.java:91`; `framework/languages/node/packages/framework/src/runtime/framework-errors-internal.ts:144`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:33` — Java 결과를 그대로 await
- 확신: 높음 — 네 runtime의 remote fine failure 18 매핑과 Kotlin 위임을 정적으로 대조했다.

추가 오류표 대조 위치: `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:933`·`framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:944`·`framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:962`·`framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:974`와 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:160`도 `NotFound`/`Unavailable`/`DeadlineExceeded`/`InvalidOperation` 등의 발생 조건을 열거한다. 이 finding의 규칙 수와 통합 초안은 확정한 queue-locality 판정에 한정했다. Immediate overload의 `DeadlineExceeded`와 Store 실패 매핑의 상충은 BLOCKERS에서 분리했다.

### F-R4-4 Yield 제공 call 목록의 중복

- 분류: consolidation
- 위치: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:55`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:99`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:541`; `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:892`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:915`
- 현재 규칙(인용): “`Yield`는 `SpotWide` User Spot과 Instance Spot에서 실행하는 Channel·Spot·Actor request, CPU·I/O worker와 Actor·Spot create·get-or-create call에만 제공한다.”
- 문제: Submit §2와 execution gate §3은 같은 call-family 표를 통째로 반복하며 Framework API도 제공·미제공 목록을 다시 적는다. Terminator 이름의 소유권, Yield eligibility, gate 반납 후 claim 수명은 별개 설명이지만 call-family 목록은 동일한 하나의 판정이다. 각 언어의 실제 오류 종류 차이는 F-R4-6의 별도 원인이다.
- 제안: 단일 소유자는 02-handler-turn-and-execution-gate.ko.md §16의 허용 call 관찰이며 §3은 그 표의 이유만 설명한다: “Yield는 SpotWide User Spot·Instance Spot turn의 request, CPU·I/O worker, Actor·Spot create·get-or-create에서만 제공하고 나머지 call family에서는 제공하지 않는다.”
- 규칙 수: before 3 → after 1 — 동일한 전체 call-family 목록 3벌 → 1벌; glossary의 Async/Yield 의미 설명과 언어별 public signature는 삭제 대상이 아니다.
- 행동 변경: 없음 — 제공되는 method와 유효한 실행 문맥은 그대로 유지한다.
- 영향: framework(C++, .NET, Java, Kotlin, Node); public signature 변경 없음. 대표 구현 위치: `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:180`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 문서 목록만 통합한다.
- 근거 코드: `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:180`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkApplicationExecutionContext.cs:43`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/handlers/ZLinkSuspendInvocationContext.java:102`; `framework/languages/node/packages/framework/src/runtime/execution/index.ts:206`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:39`
- 확신: 높음 — 목록의 반복을 확인했다. 허용 문맥 검사 위치도 다섯 언어에서 확인했다.

### F-R4-5 Node completion의 process 공유 예약 누락

- 분류: parity-gap
- 위치: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:340`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:368`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:378`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:570`; `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:90`
- 현재 규칙(인용): “이 예약은 callback이 반환할 때까지 유지한다. 진행 중 operation과 dispatcher에서 대기·실행 중인 callback을 합친 수는 4,096개를 넘지 않으므로”
- 문제: Node OperationRegistry는 인스턴스 Map의 entries.size만 4,096과 비교하고 take에서 지운 뒤 Promise를 완료한다. RawServiceMeshRuntime와 ServiceTerminalOperationRegistry가 각자 registry를 만들므로 두 registry에 각각 2,049개를 보관하는 것도 해당 검사만으로는 허용된다. 완료 전달 중인 자리까지 유지하는 process 예약은 이 경로에 없다. C++·.NET·Java는 process singleton dispatcher를 예약하며 Kotlin은 Java stage를 사용한다. Node의 Promise microtask가 inline continuation을 막는다는 사실은 공유 capacity 보장을 대신하지 않는다.
- 제안: 단일 소유자는 01-submit-and-completion.ko.md §11의 process completion admission이다: “모든 Framework service operation은 process가 공유하는 4,096개 completion 자리 중 하나를 submit 전에 확보하고 그 자리의 completion 전달이 끝날 때 반환하며, 자리가 없으면 submit 전에 CapacityExceeded로 끝난다.”
- 규칙 수: before 2 → after 1 — Node의 registry별 pending 한도와 다른 언어의 process 합산 예약이라는 두 capacity 정책 → process 예약 정책 하나; 기존 registry의 capacity 소유를 옮기는 제안이며 두 번째 한도를 덧붙이는 제안이 아니다.
- 행동 변경: 있음 — 여러 registry의 합산 부하 또는 completion backlog에서 Node가 지금 수락하는 호출을 CapacityExceeded로 거부하게 되므로 0.18.0 대상이다.
- 영향: framework(Node); C++·.NET·Java는 비교 기준이며 Kotlin은 Java runtime을 공유한다. 대표 구현 위치: `framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts:69`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 있음 — Node의 admission/terminal마다 공유 예약의 획득·반환이 필요하고 aggregate memory 한도가 달라진다. 성능 개선으로 단정하지 않으며 측정은 실행하지 않았다.
- 근거 코드: `framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts:69`·`framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts:149` / `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:152` / `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-registry.ts:618`; `framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:78`·`framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:181`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:57` / `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:46`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:22`·`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:68`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:33`; `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:363`·`framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:405` — 한 backend가 raw·stateful runtime을 함께 생성
- 확신: 높음 — Registry 생성 경로·예약·반환을 정적으로 확인했다. 공개 API로 aggregate overload를 발생시키는 실행 검증은 금지 범위라 수행하지 않았다.

### F-R4-6 허용되지 않은 Yield의 언어별 오류 차이

- 분류: parity-gap
- 위치: `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:55`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:110`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:532`; `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:895`; `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:38`; `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:52`
- 현재 규칙(인용): “지원하지 않는 문맥이면 outbound admission·queue 변경·turn 반납 없이 `InvalidOperation`으로 완료한다.”
- 문제: 같은 Channel request Yield를 owner turn 밖에서 호출하면 .NET은 Framework InvalidOperation, C++은 not_configured, Java는 NOT_CONFIGURED, Kotlin은 그 Java 오류, Node는 Kind가 없는 ZLinkConfigurationException을 발생시킨다. 각 검사는 submit 전에 있으므로 제출 순서 문제가 아니라 잘못된 실행 문맥을 configuration 부재로 분류하는 기존 공통 helper의 오류다. 로컬 오류를 언어 표준 오류로 허용하는 error model §3과 특정 Yield의 명시적 Kind 계약은 supervisor가 일반화하지 말고 이 특정 호출 기준으로 판정할 수 있다.
- 제안: 단일 오류 소유자는 07-framework-error-model.ko.md의 실행 문맥 위반 행이고 Yield 문서는 그 행을 참조한다: “등록은 유효하지만 현재 turn에서 허용되지 않은 Yield는 operation 제출과 gate 반납 전에 Framework ErrorKind.InvalidOperation으로 완료한다.”
- 규칙 수: before 3 → after 1 — 공개 InvalidOperation, 공개 NotConfigured, 별도 configuration exception이라는 세 오류 선택 규칙 → InvalidOperation 하나.
- 행동 변경: 있음 — C++·Java·Kotlin·Node의 exception 종류 또는 ErrorKind가 달라지며 .NET의 결과는 유지되므로 0.18.0 대상이다.
- 영향: framework(C++, Java, Kotlin, Node); .NET은 현재 계약을 충족한다. 대표 구현 위치: `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:180`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 기존 실패 helper의 오류 종류를 맞추는 범위이며 새 검사·retry·타이머는 필요하지 않다.
- 근거 코드: `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp:180` / `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:609`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkApplicationExecutionContext.cs:43`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelDirectCalls.java:393` / `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/handlers/ZLinkSuspendInvocationContext.java:160`; `framework/languages/node/packages/framework/src/runtime/channels/channel-clients.ts:460` / `framework/languages/node/packages/framework/src/runtime/execution/index.ts:206` / `framework/languages/node/packages/framework/src/contracts/Configuration/ConfigurationException.ts:1`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:39`
- 확신: 높음 — 실제 public call의 검사부터 생성되는 오류까지 확인했다. Runtime 수정은 하지 않았다.

### F-R4-7 Payload·relocation의 retained Core credit lease 잔재

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/socket/README.ko.md:1306`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:34`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:144`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:374`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:246`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:258`; `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:101`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1274`; `framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:168`; `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:617`
- 현재 규칙(인용): “Core receive에서 retain한 record는 payload와 Core receive-credit lease의 shared owner 하나를 가진다.” / “Framework는 retained-credit lease를 요청하거나 Core byte charge를 handler·reply lifetime까지 연장하지 않는다.”
- 문제: 현재 Core HWM은 frame이 queue를 떠나면 charge를 반환한다. Backpressure 문서는 그 경계를 명시하지만 payload §8은 마지막 child/reply terminal까지 Core lease를 보관하라고 하며 relocation flow와 wire 문서도 chunk 복사 후 Core lease 반환을 지시한다. 이는 일반 payload storage의 수명과 Core queue credit을 같은 소유권으로 보던 과거 모델의 잔재다. Node ingress owner와 .NET RawIngressOwnership은 일반 record·queue permit을 소유하고, Java chunk 경로도 배열을 복사할 뿐 그 코드에 별도 Core-credit lease handle은 없다.
- 제안: Core credit의 단일 소유자는 core socket HWM 계약이며 Framework payload §8은 일반 storage 수명만 설명한다: “Core receive credit은 record가 Core queue를 떠날 때 반환되며, 이후 Framework가 보관하는 payload와 child permit의 수명은 Core credit의 수명을 연장하지 않는다.”
- 규칙 수: before 5 → after 1 — Core, Framework backpressure, payload, relocation flow, wire의 credit 반환 규정 5곳 → Core 계약 1곳; child payload ref-count와 application permit 규칙은 별개로 유지한다.
- 행동 변경: 없음 — 현재 dequeue credit 계약에 맞춰 문서의 존재하지 않는 lease만 제거한다. Chunk 복사나 payload 해제 시점을 코드에서 바꾸는 제안은 아니다.
- 영향: core(계약 참조), framework(C++, .NET, Java, Kotlin, Node). C++·Java의 모든 binding receive wrapper와 Kotlin 독립 payload 경로는 미검증이다. 대표 구현 위치: `framework/languages/node/packages/framework/src/runtime/application-jobs/application-ingress-record-owner.ts:138`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 코드의 복사·참조 횟수는 바꾸지 않는다. Lease를 구현해서 추가하려는 작업이 생기지 않도록 계약을 정리하는 범위다.
- 근거 코드: `framework/languages/node/packages/framework/src/runtime/application-jobs/application-ingress-record-owner.ts:138`·`framework/languages/node/packages/framework/src/runtime/application-jobs/application-ingress-record-owner.ts:200`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:4993`·`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5015`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkRelocationPayloadTransfer.java:272`; `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:2739` — ordinary receive 경계; native credit 내부 계상은 코드 미검증
- 확신: 높음 — Spec 간 직접 충돌은 확정이다. Native credit의 실행 검증이나 복사 제거의 안전성까지 판정하지 않았다.

### F-R4-8 Framework service control의 물리 lane 배정 불일치

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:559`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:472`; `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:403`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:127`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:137`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:280`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:964`; `core/doc/spec/core/socket/README.ko.md:1302`; `core/doc/spec/core/socket/README.ko.md:1415`
- 현재 규칙(인용): “liveness·admission·relocation·reply recovery service control은 ROUTER-ROUTER Completion connection에서 받는다.”
- 문제: 세 문서는 Framework service control 전체를 Completion connection에 배치하지만 backpressure §3은 heartbeat·topology·relocation·SendReady를 ordinary application data lane으로 분류한다. Core의 terminal REPLY와 receive-flow PAUSED/RUNNING 제어는 Framework Hello·relocation service-wire message와 다르다. C++은 ordinary permit이 없으면 control도 receive하지 않으며 Node와 .NET도 ordinary receive 결과를 분류한다. Infrastructure execution domain에서 처리한다는 사실이 물리 Completion connection을 뜻하지 않는다.
- 제안: Framework ingress 분류의 단일 소유자는 04-application-job-queue-and-backpressure.ko.md §3이다: “Framework service-wire control은 ordinary ingress로 수신하고, Core가 식별하는 terminal reply/error completion과 Core receive-flow control만 Core의 topology별 완료·제어 경로를 따른다.”
- 규칙 수: before 4 → after 1 — 서로 충돌하는 Framework lane 배정 설명 4곳 → ingress 경계 1곳; Core 물리 lane 규칙을 Framework에서 새로 정하지 않는다.
- 행동 변경: 없음 — 현재 binding 수신 경로에 맞춰 문서의 lane 배정만 고친다. Service control을 다른 socket이나 queue로 옮기는 runtime 변경은 없다.
- 영향: framework(C++, .NET, Java, Kotlin, Node); Core·binding의 lane 배정은 변경하지 않는다. 대표 구현 위치: `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:2730`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 새 completion/control queue나 poller를 만들지 않는다.
- 근거 코드: `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:2730`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:4993`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:4249`·`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:4281`; `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:595`·`framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:1594`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:33` — Java runtime 투영; Kotlin 전용 물리 수신기는 확인하지 않음
- 확신: 높음 — 네 runtime의 ordinary receive 분류와 Core·Framework 경계 문장을 대조했다.

### F-R4-9 Execution domain과 owner FIFO의 혼동

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:24`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:324`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:479`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:481`; `framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:33`
- 현재 규칙(인용): “두 도메인은 owner마다 물리적으로 다른 FIFO로 분리된다”
- 문제: §13의 ‘두 도메인’은 application/infrastructure인데 참조한 §7의 두 FIFO는 application/lifecycle이다. 바로 앞 479행은 lifecycle user callback이 application domain이라고 명시한다. 도메인은 진행 독립성을, FIFO lane은 owner 안에서의 작업 분류와 capacity를 정하므로 같은 두 구획이 아니다. 실제 serial queue는 application/lifecycle를 분리하고 infrastructure는 그 queue 밖의 수신·completion 실행 경로에서도 진행한다.
- 제안: 단일 소유자는 02-handler-turn-and-execution-gate.ko.md §1의 영역 관계다: “Application과 infrastructure는 독립적으로 진행하는 실행 영역이며, application callback을 실행하는 owner의 application·lifecycle FIFO는 그 영역 구분과 별개인 §7의 작업 분류다.”
- 규칙 수: before 2 → after 1 — 두 축을 같다고 하는 설명과 다르다고 하는 설명 2개 → 관계 정의 1개.
- 행동 변경: 없음 — 현재 실행 경로를 재배치하지 않고 잘못된 FIFO 대응 문장만 정리한다.
- 영향: framework(C++, .NET, Java, Kotlin, Node). Kotlin의 별도 state lane도 이 domain/FIFO와 다른 단위다. 대표 구현 위치: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:26`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — Queue·executor 개수나 scheduling policy 변경 없음.
- 근거 코드: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:26`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueue.java:57`; `framework/languages/node/packages/framework/src/runtime/execution/serial-execution-queue.ts:73`; `framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:50` — owner FIFO 밖의 completion dispatcher; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/execution/ZLinkStateLane.kt:16`
- 확신: 높음 — 문서 자체가 서로 다른 분류를 같은 표로 연결하며, 구현에서도 단위가 다름을 확인했다.

### F-R4-10 Lifecycle generation의 수명 정의 불일치

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md:12`; `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md:342`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1510`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1516`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1519`
- 현재 규칙(인용): “node lifecycle generation | 재시작마다 증가” / “0이 아닌 opaque equality token. 숫자 크기로 실행 순서를 판단하지 않는다.”
- 문제: Layering은 식별자의 형식·수명 소유자가 glossary라고 선언한 뒤 lifecycle generation을 증가하는 번호로 다시 정의한다. 실제 .NET과 Node 발급은 난수이며 Java 수신도 equality token으로 취급한다. ObjectGeneration의 counter 규칙을 lifecycle generation에 가져온 설명이다.
- 제안: 단일 소유자는 glossary의 Lifecycle generation이다: “Lifecycle generation은 실행을 구별하는 0이 아닌 opaque equality token이며 새 lifecycle에는 다른 값을 사용하고 숫자의 대소로 실행 순서를 판정하지 않는다.”
- 규칙 수: before 2 → after 1 — 순서 증가 정의와 equality 정의 2개 → equality 정의 1개.
- 행동 변경: 없음 — Runtime 발급·비교를 바꾸지 않는다.
- 영향: framework(.NET, Node, Java). C++ 발급 경로와 Kotlin의 독립 발급 여부는 미검증이다. 대표 구현 위치: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:118`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 문서의 증가 표현만 제거한다.
- 근거 코드: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:118`·`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:11340`; `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:2641`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:4293`
- 확신: 높음 — 두 실제 발급기와 Java 수신 비교를 확인했다. 발급 허용 bit 범위는 이 finding의 변경 대상이 아니다.

### F-R4-11 State lane 재진입 검사의 release 예외

- 분류: spec-impl-drift
- 위치: `framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:368`; `framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md:518`; `framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md:527`; `framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md:530`
- 현재 규칙(인용): “`throwIfReentrant`는 **선택이 아니라 필수 계약이다.**” / “디버그 빌드에서는 필수다. 릴리스 빌드에서는 선택이되”
- 문제: State lane의 동일 재진입 판정에 무조건 필수와 과거 재귀 lock 사용 이력에 따른 release 예외가 겹친다. 확인한 C++·.NET·Java·Node·Kotlin의 state-lane 진입은 build mode 분기 없이 검사한다. 과거 구현 이력으로 현재 계약을 달리하는 조건은 필요하지 않다. 일반적인 isOnLane 진단 assertion 전체를 무조건 켜라는 제안은 아니다.
- 제안: 단일 소유자는 06-state-ownership-and-lanes.ko.md §7의 state-lane 진입 계약이다: “같은 state lane의 현재 turn을 기다리는 재진입은 build mode와 과거 lock 구현에 관계없이 진입 지점에서 거부한다.”
- 규칙 수: before 2 → after 1 — 필수 규칙과 release/history 예외 규칙 2개 → 필수 규칙 1개.
- 행동 변경: 없음 — 현재 다섯 언어에 이미 있는 검사를 유지하는 문서 수정이다.
- 영향: framework(C++, .NET, Java, Kotlin, Node). State lane의 기다리는 진입 경계에 한정한다. 대표 구현 위치: `framework/languages/cpp/framework/src/runtime/execution/state_lane.cpp:40`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 기존 검사를 추가하거나 제거하지 않는다.
- 근거 코드: `framework/languages/cpp/framework/src/runtime/execution/state_lane.cpp:40`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkStateLane.cs:68`·`framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkStateLane.cs:119`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/execution/ZLinkStateLane.java:59`·`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/execution/ZLinkStateLane.java:106`; `framework/languages/node/packages/framework/src/runtime/execution/state-lane.ts:30`·`framework/languages/node/packages/framework/src/runtime/execution/state-lane.ts:66`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/execution/ZLinkStateLane.kt:41`·`framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/execution/ZLinkStateLane.kt:71`
- 확신: 높음 — 제안 범위의 검사에 build 조건이 없다는 것을 다섯 언어에서 확인했다.

### F-R4-12 용어집 항목의 spec 링크 미참조

- 분류: form
- 위치: `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:42`; `doc/principal/documentation/spec-writing-guide.ko.md:219`; 아래 13개 항목의 정의 위치
- 현재 규칙(인용): “개별 스펙은 그 용어를 처음 쓰는 자리에서 현재 문맥에 맞는 한 문장으로 소개한 뒤 이 문서의 항목에 링크한다”
- 문제: Core·binding·Framework common spec의 Markdown 393개를 대상으로 한국어 glossary를 가리키는 fragment를 검색하고 132개 ### 항목의 명시적 anchor와 자동 heading slug를 함께 대조했다. Glossary 자기 참조를 제외하면 13개 항목의 incoming link가 0이다. 한국어 common spec 144개로 한정해도 결과는 같다. 이 목록은 코드에서 쓰이지 않는다는 판정이 아니라 spec에서 정의에 도달할 링크가 없다는 목록이다. 단순 이름 검색으로 삭제를 권하지 않는다.
- 제안: 단일 소유자는 glossary의 기존 각 항목이다: “공유 용어의 정의는 용어집에 하나만 두고 해당 개념을 소개하는 spec의 첫 사용에서 그 항목을 연결한다.”
- 규칙 수: before 132 → after 132 — 정의 132개 → 132개, 새 행동 규칙 0개; 링크 미참조 정의 13개 → 0개를 목표로 한다. 실제로 폐기할 개념의 선택은 supervisor 소관이다.
- 행동 변경: 없음 — 정의에 도달하는 문서 링크만 보완한다.
- 영향: framework(C++, .NET, Java, Kotlin, Node)의 공통 문서; 언어별 runtime 동작은 이 링크 finding에서 검증 대상이 아니다.
- 성능 영향: 없음 — 없음.
- 근거 코드: 없음 — 실행 코드가 아니라 spec 링크 그래프의 전수 정적 대조이며 언어별 runtime 미사용 여부를 주장하지 않는다.
- 확신: 높음 — 분모·검색 경로·자동 slug 포함 여부를 고정한 집계다. Fragment가 없는 glossary 문서 단위 링크는 특정 용어의 incoming link로 세지 않았다.

링크 미참조 13개:

- Bounded aggregate commit — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:656`
- Durable activation inbox — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:799`
- Activation recovery pointer — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:829`
- Recovery receipt — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:844`
- TargetNotFound — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1003`
- RouteNotConnected — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1009`
- 재전송 창 (Cutover retransmission window) — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1295`
- Channel Client와 Server role — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1430`
- Routing ID prefix — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:2078`
- CSPRNG — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:2092`
- RoutingIdConflict — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:2105`
- SpotIdConflict — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:2118`
- Session Actor 위치 갱신 — `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:2176`

### F-R4-13 공통 실행 용어의 용어집 미등록

- 분류: form
- 위치: `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:42`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:52`; 아래 9개 용어의 사용·도입 위치
- 현재 규칙(인용): “이 용어집은 여러 스펙이 공유하는 domain term 정의의 단일 기준이다.”
- 문제: R4 본문에서 실행 단위·진행 주체·완료 경계로 쓰이는 개념을 132개 glossary heading 및 alias와 대조했다. 아래 9개는 독립 정의나 해당 개념으로 연결되는 alias가 없다. 단어가 본문에 한 번 나온 경우와 정의가 등록된 경우를 구분했다. FIFO·JSON·thread 같은 일반 기술어, 단순 method/field 이름은 이 목록에서 제외했다.
- 제안: 단일 정의 위치는 glossary이며 상세 동작은 기존 주제 문서가 계속 소유한다: “아래 공통 실행 용어는 기존 개념의 범위와 수명만 용어집에서 정의하고, 본문의 최초 소개는 그 정의를 참조한다.”
- 규칙 수: before 9 → after 9 — 기존 개념 9개 → 9개, 새 실행 규칙 0개. 이미 같은 개념을 정의한 항목은 alias로 연결하고 상세 동작을 용어집에 복제하지 않는다.
- 행동 변경: 없음 — 용어의 정의 위치와 링크만 보완한다.
- 영향: framework(C++, .NET, Java, Kotlin, Node)의 공통 문서. Runtime 개념 자체를 추가하지 않는다. 대표 구현 위치: `framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.hpp:70`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 없음.
- 근거 코드: `framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.hpp:70`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:26`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:22`; `framework/languages/node/packages/framework/src/runtime/execution/serial-execution-queue.ts:73`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/execution/ZLinkStateLane.kt:16`
- 확신: 높음 — 아래 열거한 9개에 대한 미등록은 확인했다. 본문에 등장하는 모든 자연어 명사를 빠짐없이 용어 후보로 간주한 수치는 아니다.

미등록 9개:

| 용어 | 사용·도입 위치 | 대조 결과 |
|---|---|---|
| Actor | `framework/doc/framework/common/spec/server/00-foundation/03-overview.ko.md:15`; `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:15` | Actor membership 항목은 있으나 Actor 자체의 정의 항목은 없음 |
| execution gate | `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:350` | Spot turn 정의의 하위 표현으로 등장하지만 독립 항목/alias 없음 |
| handler turn | `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:169`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:83` | Spot turn 항목만으로 Node·Session을 포함하는 handler turn의 범위가 정해지지 않음 |
| state lane | `framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:30` | glossary 본문에도 해당 문자열 없음 |
| application lane | `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:328` | relocation hold 설명에 단어만 등장하며 정의 항목 없음 |
| lifecycle lane | `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:329` | glossary 본문에도 해당 문자열 없음 |
| completion dispatcher | `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:368` | pending registry와 다른 completion 전달 주체이나 항목 없음 |
| source-local admission | `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:225`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:108` | One-way 정상 완료 항목에 범위를 연결할 alias/정의가 없음 |
| 양보 부채 | `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:352` | 실행 우선순위를 설명하는 고유 개념이나 항목 없음 |

### F-R4-14 실행 queue owner와 MeshNode Owner의 혼동

- 분류: form
- 위치: `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:330`; `framework/doc/framework/common/spec/server/00-foundation/03-overview.ko.md:120`; `framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:37`; `framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:43`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:339`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:324`
- 현재 규칙(인용): “현재 Actor·Spot을 실행하는 MeshNode별 대기열”
- 문제: Glossary Owner는 authority가 가리키는 MeshNode다. 그런데 execution lane은 Spot·Actor 등의 실행 객체마다 있고, overview의 owner 표에는 Node·Spot·Actor·Session을 함께 나열하면서 MeshNode Owner로 링크한다. Backpressure 한도 표는 이 잘못된 짧은 소개를 ‘MeshNode별 대기열’이라는 실제 granularity 규칙으로 확장한다. 이대로 읽으면 같은 MeshNode의 모든 Actor가 하나의 owner capacity를 공유한다고 오해하게 된다.
- 제안: 단일 queue granularity 소유자는 02-handler-turn-and-execution-gate.ko.md §7이다: “Application·lifecycle FIFO는 Spot·Actor 등 실행 객체마다 두며, Location Store authority가 가리키는 MeshNode Owner와 구별한다.”
- 규칙 수: before 2 → after 1 — MeshNode별과 실행 객체별이라는 상충하는 FIFO 단위 2개 → 실행 객체별 1개; Location Store Owner 정의는 유지한다.
- 행동 변경: 없음 — 현재 queue 생성·capacity 적용 단위를 유지하고 잘못된 소개·링크를 정정한다.
- 영향: framework(C++, .NET, Java, Kotlin, Node). 대표 구현 위치: `framework/languages/node/packages/framework/src/runtime/execution/serial-execution-queue.ts:73`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — Queue를 합치거나 재분할하지 않는다.
- 근거 코드: `framework/languages/node/packages/framework/src/runtime/execution/serial-execution-queue.ts:73`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:26`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueue.java:49`; `framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.hpp:73`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/execution/ZLinkStateLane.kt:16`
- 확신: 높음 — Spec의 두 granularity와 실제 owner queue 정책을 확인했다.

### F-R4-15 Backpressure 정의의 remote receive-flow 누락

- 분류: form
- 위치: `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:943`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:987`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:267`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:280`; `core/doc/spec/core/socket/README.ko.md:1415`
- 현재 규칙(인용): “remote의 지연은 별도 신호가 아니라 송신 대기로 전달된다.”
- 문제: Glossary는 local 송신 queue HWM만으로 backpressure를 정의하지만 실행 문서는 host permit pressure를 PAUSED/RUNNING으로 Core에 전달하고 remote-pause blocker와 local HWM blocker를 독립적으로 합성한다고 명시한다. HWM 값 자체가 local 설정이라는 설명은 맞아도 remote 흐름 제어 신호를 배제하는 결론은 현재 receive-flow 계약과 다르다.
- 제안: 단일 정의는 glossary Backpressure이며 상세 합성은 Core 계약으로 연결한다: “Backpressure는 Core가 local byte HWM과 remote receive-flow 상태를 반영해 새 송신 admission을 제한하는 흐름 제어이며, Framework는 자신의 permit pressure를 공개 receive-flow API에 전달한다.”
- 규칙 수: before 2 → after 1 — local-only 정의와 local/remote 합성 정의 2개 → 합성 경계를 설명하는 정의 1개.
- 행동 변경: 없음 — 현재 PAUSED/RUNNING 동작과 HWM 설정을 유지한다.
- 영향: framework(C++, .NET, Java, Kotlin, Node), core(receive-flow 계약 참조). 대표 구현 위치: `framework/languages/cpp/framework/src/runtime/dispatch/application_job_queue.hpp:84`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 새 상태·신호·주기 조회를 도입하지 않는다.
- 근거 코드: `framework/languages/cpp/framework/src/runtime/dispatch/application_job_queue.hpp:84`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:415`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/dispatch/ZLinkApplicationJobReceiveFlowController.java:113`; `framework/languages/node/packages/framework/src/runtime/application-jobs/receive-flow-controller.ts:145`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt:33` — Java runtime 공유; Kotlin 전용 flow controller는 미검증
- 확신: 높음 — 세 binding-facing setter 경로와 C++ setter dispatch를 확인했다.

### F-R4-16 Reply token 정의의 STREAM 적용 범위

- 분류: form
- 위치: `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1911`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1917`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1920`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:248`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:258`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:463`; `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:946`
- 현재 규칙(인용): “최종 reply를 만든 뒤 다시 사용할 수 없다.” / “첫 terminal reply, error 또는 request cancellation·timeout으로 닫힌다.”
- 문제: STREAM 절이 glossary reply-token을 직접 링크하지만 glossary는 typed handler의 단일 반환을 표기로 삼고 request timeout 때 token이 닫힌다고 범위 없이 설명한다. STREAM은 첫 유효 terminator가 transport 전에 token을 소비하며 remote caller request timeout은 wire에 없어 token의 admission deadline이 아니다. 같은 ‘reply token’ 소개가 서로 다른 표면의 수명 규칙을 합쳐 놓았다.
- 제안: STREAM token 수명은 01-submit-and-completion.ko.md §8이 소유하고 glossary는 그 문맥을 명시한다: “STREAM reply token은 첫 유효 terminator가 transport admission 전에 소비하는 one-shot capability이며 remote caller의 request timeout으로 responder의 admission deadline을 정하지 않는다.”
- 규칙 수: before 2 → after 1 — STREAM에 동시에 적용될 수 있게 쓰인 두 수명 설명 → STREAM 수명 규칙 1개; 다른 topology의 typed reply 계약은 변경하지 않는다.
- 행동 변경: 없음 — STREAM의 기존 token claim과 timeout 처리는 그대로 두고 glossary의 적용 범위만 명확히 한다.
- 영향: framework(C++, .NET, Java, Node). Kotlin의 독립 STREAM reply 표면은 미검증이다. 대표 구현 위치: `framework/languages/cpp/framework/src/runtime/streams/stream_runtime.cpp:126`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — Token state나 타이머를 추가하지 않는다.
- 근거 코드: `framework/languages/cpp/framework/src/runtime/streams/stream_runtime.cpp:126`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Streams/ZLinkSessionStreamCalls.cs:168`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamSessionCalls.java:312`; `framework/languages/node/packages/framework/src/runtime/streams/session-calls.ts:190` / `framework/languages/node/packages/framework/src/runtime/streams/session-context.ts:301`
- 확신: 중간 — 네 언어의 claim 경계는 확인했다. Kotlin 전용 STREAM reply wrapper와 모든 ClientServer token 수명은 미검증이며 glossary의 원래 의도는 supervisor 확인이 필요하다.

### F-R4-17 Spot application queue의 control 분류 불일치

- 분류: form
- 위치: `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1147`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1155`; `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md:726`; `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md:730`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:326`; `framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:45`
- 현재 규칙(인용): “Spot direct, matching publish, timer와 Spot control work item을 한 순서로 보관한다.”
- 문제: Glossary는 Spot control까지 한 순서로 보관한다고 정의하고 Spot messaging 표의 짧은 소개도 이 문장을 반복한다. 그러나 같은 표의 제외 열과 바로 뒤 설명은 join·leave·lifecycle control을 별도 Spot control claim으로 보낸다. 실행 문서는 application/lifecycle의 별도 FIFO·별도 한도를 정한다. 물리 queue를 가리키는 용어의 정의가 control claim 분리 전 설명을 유지한 것이다.
- 제안: 작업 분류의 단일 소유자는 02-handler-turn-and-execution-gate.ko.md §7이며 glossary는 그 분류로 연결한다: “Spot application queue는 Spot 업무 payload와 timer를 보관하고 join·leave·relocation 등 lifecycle control은 별도 lifecycle lane의 control claim으로 처리한다.”
- 규칙 수: before 3 → after 1 — Glossary의 포함 정의, Spot messaging의 포함 소개, execution의 분리 정의 3개 → 작업 분류 1개.
- 행동 변경: 없음 — 이미 분리된 두 FIFO와 control claim의 capacity·우선순위를 유지한다.
- 영향: framework(C++, .NET, Java, Kotlin, Node). 대표 구현 위치: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:26`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — Queue·claim·reservation의 생성 또는 삭제가 없다.
- 근거 코드: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:26`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueue.java:57`; `framework/languages/node/packages/framework/src/runtime/execution/serial-execution-queue.ts:78`; `framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.cpp:1043` — lifecycle 선택; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/execution/ZLinkStateLane.kt:20` — Kotlin state lane과 Java serial execution은 별개
- 확신: 높음 — 문서 표 안의 직접 충돌과 네 runtime의 FIFO 분리를 확인했다.

### F-R4-18 Snapshot 정의의 anchor 충돌

- 분류: form
- 위치: `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:52`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1079`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1080`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1132`; `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:168`; `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:364`; `framework/doc/framework/common/spec/server/05-location-relocation/02-location-store-redis.ko.md:46`
- 현재 규칙(인용): “Publish target snapshot” / “특정 시점의 runtime 상태를 읽기 전용 값으로 복사한 결과다.”
- 문제: 명시적 <a id="snapshot">는 Publish target snapshot 앞에 있고 뒤의 ### Snapshot도 자동 slug snapshot을 만든다. 서로 다른 두 정의가 같은 fragment를 사용한다. Metadata snapshot이나 descriptor weight snapshot을 소개하는 문장도 #snapshot으로 링크하므로 publish target 목록의 수명 정의로 도착할 수 있다. 링크 누락 검사만으로는 발견되지 않는 의미 충돌이다. 한국어 common spec의 외부 #snapshot 링크는 18곳이다.
- 제안: Glossary의 일반 Snapshot 한 항목이 정의를 소유하고 기존 snapshot·publish-target-snapshot 링크는 그 항목으로 수렴시킨다: “Snapshot은 특정 시점에 고정한 읽기 전용 값이며 monitoring·metadata·publish target별 생성 시점과 수명은 각 기능 계약을 따른다.”
- 규칙 수: before 2 → after 1 — 일반·publish 특수형을 각각 완전 정의한 2개 → 공통 정의 1개; publish의 구체 target-selection 규칙은 channel messaging이 계속 소유한다.
- 행동 변경: 없음 — 문서 anchor의 대상만 명확히 하고 runtime snapshot 수명은 바꾸지 않는다.
- 영향: Framework 공통·언어별 문서. Runtime 언어별 동작을 변경하지 않는다.
- 성능 영향: 없음 — 없음.
- 근거 코드: 없음 — 문서의 HTML anchor와 Markdown heading slug 충돌을 정적으로 확인한 finding이다. 이 finding에서는 언어별 runtime snapshot 구현을 검증하지 않았다.
- 확신: 높음 — 두 id 생성 위치와 18개 incoming 링크를 확인했다. 사이트 빌드·렌더러 실행은 수행하지 않았다.

외부 `#snapshot` 링크 18곳(링크가 있는 행을 확인했으며 R4 밖의 모든 문서를 완독한 것은 아님):

- `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:398`
- `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:282`
- `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:168`
- `framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md:693`
- `framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:327`
- `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md:364`
- `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:293`
- `framework/doc/framework/common/spec/server/05-location-relocation/02-location-store-redis.ko.md:46`
- `framework/doc/framework/common/spec/server/languages/dotnet/README.ko.md:30`
- `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/06-actors.ko.md:338`
- `framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md:278`
- `framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md:273`
- `framework/doc/framework/common/spec/server/languages/node/interfaces/05-actors.ko.md:181`
- `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:348`
- `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/configuration-host.ko.md:195`
- `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md:62`
- `framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.ko.md:38`
- `framework/doc/framework/common/spec/stream-connector/languages/dotnet/03-stream-connector.ko.md:26`

### F-R4-19 Payload 검증 요구의 white-box 조건 중복

- 분류: form
- 위치: `doc/principal/documentation/spec-writing-guide.ko.md:701`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:34`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:50`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:199`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:211`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:223`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:228`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:271`; `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:301`
- 현재 규칙(인용): “내부 확인 조건(측정 기반, 공개 표면으로는 확인할 수 없음)”
- 문제: §9는 공개 표면만으로 검증한다고 시작한 뒤 ‘공개 표면으로는 확인할 수 없음’이라는 별도 목록 8개를 둔다. 같은 payload 이중 보관, byte 왕복, accessor 복사, relocation 기록 시점, codec table/lock, 송신 cache 1,024개, cache 한도 뒤 평가, per-message 객체 생성은 이미 §2·§3·§7이 소유한 내부 비용 규칙이다. 내부 조건이라는 라벨은 guide §9.3의 interface-only 요구를 충족시키지 않는다.
- 제안: 내부 조건은 05-payload-ownership-and-codec.ko.md §2·§3·§7의 각 원래 규칙만 소유한다: “복사·할당·cache 구조의 내부 측정 조건은 해당 구현 규칙에서 한 번만 정의하고 마지막 검증 요구에는 typed 값·오류·해제 등 공개 인터페이스에서 관찰되는 결과만 둔다.”
- 규칙 수: before 16 → after 8 — 내부 비용 규칙 8개와 검증 절의 반복 8개 → 원래 내부 규칙 8개; 비용 요구 자체를 삭제하지 않는다.
- 행동 변경: 없음 — Test의 assertion이나 runtime 비용 계약을 낮추지 않고 중복 문장만 원래 소유 절로 정리한다.
- 영향: Framework 공통 문서. C++·Node codec 구현 일부를 비용 규칙의 구현 예로 확인했고 .NET·Java·Kotlin codec cache는 이 finding에서 미검증이다. 대표 구현 위치: `framework/languages/cpp/framework/src/runtime/codecs/serializer.cpp:180`; 언어별 위치는 아래 근거 코드 참조.
- 성능 영향: 없음 — 측정 항목을 없애거나 코드의 복사·cache 정책을 바꾸지 않는다.
- 근거 코드: `framework/languages/cpp/framework/src/runtime/codecs/serializer.cpp:180`·`framework/languages/cpp/framework/src/runtime/codecs/serializer.cpp:199`; `framework/languages/node/packages/framework/src/runtime/messaging/payload-codec.ts:293`·`framework/languages/node/packages/framework/src/runtime/messaging/payload-codec.ts:310`
- 확신: 높음 — 8개 bullet과 해당 본문 내부 규칙의 반복을 확인했다. 할당량 측정은 금지 범위라 수행하지 않았다.

## 용어집 대조 집계

정의 항목의 존재, incoming fragment 링크, 소개 문장의 의미를 각각 대조했다. ‘미참조’는 실행 코드에서의 미사용을 뜻하지 않는다.

| 집계 대상 | 수 | 목록 |
|---|---:|---|
| 한국어 glossary의 `###` 정의 항목 | 132 | F-R4-12의 분모 |
| 외부 spec의 항목 링크가 없는 정의 | 13 | F-R4-12의 전수 목록 |
| R4에서 확인한 미등록 공통 실행 용어 | 9 | F-R4-13의 목록; 일반 기술어·단순 method/field 제외 |
| 정의와 소개 문장의 범위·의미가 다른 용어 | 6 | 아래 표 |
| 서로 다른 정의가 같은 anchor를 쓰는 용어 | 1 | Snapshot, F-R4-18; 한국어 incoming 링크 18곳 |

| 용어 | 차이 | 정의·소개 위치 | 연결 finding |
|---|---|---|---|
| Owner | authority의 MeshNode와 execution object의 FIFO owner가 섞임 | `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:330`; `framework/doc/framework/common/spec/server/00-foundation/03-overview.ko.md:120`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:339` | F-R4-14 |
| Backpressure | local HWM만 설명하는 정의와 remote receive-flow 합성이 다름 | `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:987`; `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:267` | F-R4-15 |
| Reply token | 일반 terminal·timeout 정의를 STREAM claim·admission 수명에 연결 | `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1917`; `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:248` | F-R4-16 |
| Spot application queue | control 포함 소개와 lifecycle control 분리가 다름 | `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1155`; `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md:726`; `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:326` | F-R4-17 |
| Lifecycle generation | opaque equality token과 재시작 증가 번호가 다름 | `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1516`; `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md:342` | F-R4-10 |
| Membership | Actor의 Spot 소속을 Mesh·Channel registration 항목으로 연결 | `framework/doc/framework/common/spec/server/00-foundation/03-overview.ko.md:123`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1418`; `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:141` | 추가 후보 |

미등록 9개와 의미 불일치 6개는 확인한 제품 개념의 목록이다. 모든 자연어 명사를 용어로 세거나 모든 외부 spec 본문을 의미 분석한 전수 어휘 집계는 아니다. 반면 incoming 링크 0인 13개는 검색 범위를 고정한 전수 집계다.

## 추가 후보(요약 1줄)

- `form`, 행동 변경 없음 — Actor `membership`의 소개 `framework/doc/framework/common/spec/server/00-foundation/03-overview.ko.md:123`가 topology Membership `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1418`을 가리킨다; 기존 Actor membership `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:141`으로 연결하면 참조 대상 2개 → 1개이며 초안은 “Actor가 Spot에 속하는 관계는 Actor membership 정의를 따른다.”이다. Runtime membership 동작은 이 링크 후보에서 검증하지 않았다.

## 판정 범위와 미승격 항목

- `08-layering`의 startup 검사 `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md:320`는 Framework 등록 이름·handler·role의 조합에 대한 것이다. 이 부분에서 Core가 검증한 값을 Framework가 다시 검증한다는 구현 증거는 확보하지 못했으므로 별도 lower-layer finding을 만들지 않았다. Core credit 및 physical lane 경계의 문서 충돌은 F-R4-7·8에 한정했다.
- Binding native completion과 Framework service-wire completion은 서로 다른 operation을 소유한다. F-R4-1은 native cancellation 계약의 중복 문장에 대한 것이며 두 registry를 합치거나 Framework의 service correlation을 없애는 제안이 아니다.
- F-R4-5·6은 정적 호출 경로 대조에서 확인한 차이다. Public API 실행 repro, contract test의 통과 여부, 성능 수치는 보고하지 않는다. `gate-drift`로 확정한 항목은 없다.
- R4 본문은 독자 질문, 굵은 규칙과 이유, 계약·구현 서술의 구별, 한 규칙의 계층 소유권, 검증 절의 interface 관찰 여부를 함께 읽었다. 형식만으로 runtime parity를 추정하지 않았으며 별도 원인이 확인된 위반을 finding으로 남겼다.

## 읽은 범위

아래 표는 본문을 읽은 파일과 확인 구간이다. 범위 밖의 전체 파일을 읽은 것으로 계수하지 않는다. 중복해서 읽은 행은 각 표의 행 수에 한 번만 포함했다. `rg`의 매칭 행만 본 파일·변경 diff는 뒤에서 별도로 구분한다.

### 지정 스펙 — 17개 파일, 8,773행 전부

| 파일 | 읽은 행 | 읽은 행 수 |
|---|---|---:|
| `framework/doc/framework/common/spec/server/00-foundation/01-public-contract-governance.ko.md` | 1–206 | 206 |
| `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md` | 1–2305 | 2305 |
| `framework/doc/framework/common/spec/server/00-foundation/03-overview.ko.md` | 1–186 | 186 |
| `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md` | 1–621 | 621 |
| `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md` | 1–269 | 269 |
| `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md` | 1–1131 | 1131 |
| `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md` | 1–181 | 181 |
| `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md` | 1–474 | 474 |
| `framework/doc/framework/common/spec/server/00-foundation/README.ko.md` | 1–110 | 110 |
| `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` | 1–579 | 579 |
| `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md` | 1–586 | 586 |
| `framework/doc/framework/common/spec/server/01-execution/03-cancellation-and-shutdown.ko.md` | 1–208 | 208 |
| `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md` | 1–434 | 434 |
| `framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md` | 1–315 | 315 |
| `framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md` | 1–428 | 428 |
| `framework/doc/framework/common/spec/server/01-execution/07-serial-executor-layers.ko.md` | 1–612 | 612 |
| `framework/doc/framework/common/spec/server/01-execution/README.ko.md` | 1–128 | 128 |

### 기준 문서와 인접 계약

| 파일 | 읽은 행 | 읽은 행 수 |
|---|---|---:|
| `AGENTS.md` | 1–137 | 137 |
| `doc/AGENTS.md` | 1–51 | 51 |
| `framework/AGENTS.md` | 1–128 | 128 |
| `framework/doc/AGENTS.md` | 1–30 | 30 |
| `framework/languages/dotnet/AGENTS.md` | 1–7 | 7 |
| `doc/plan/c016-worklog/spec-review/README.ko.md` | 1–66 | 66 |
| `doc/principal/documentation/spec-writing-guide.ko.md` | 1–418, 674–755 | 500 |
| `doc/principal/documentation/documentation-principles.ko.md` | 1–474 | 474 |
| `doc/plan/c016-worklog/decisions.ko.md` | 1205–1318, 1343–1378 | 150 |
| `bindings/doc/spec/async-execution-model.ko.md` | 1–155 | 155 |
| `bindings/doc/spec/async-coroutine-policy.ko.md` | 1–154 | 154 |
| `framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md` | 347–470, 477–489 | 137 |
| `framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md` | 712–783 | 72 |
| `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md` | 15–35 | 21 |
| `framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md` | 759–862 | 104 |
| `framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md` | 155–175 | 21 |
| `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md` | 607–629 | 23 |
| `core/doc/spec/core/socket/README.ko.md` | 1294–1310, 1404–1423 | 37 |

Core socket README는 시작 HEAD에서도 1288–1304, 1399–1417행을 읽었다. 다른 작업의 변경 뒤 현재 행 번호로 다시 확인한 구간을 표에 적었다. `decisions.ko.md`의 D-090부터 D-101까지는 위 구간에 포함되며, 도중의 다른 결정 번호를 건너뛰어 새 계약으로 추정하지 않았다.

### 구현 확인 — C++

| 파일 | 읽은 행 | 읽은 행 수 |
|---|---|---:|
| `framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp` | 1–235 | 235 |
| `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp` | 1312–1343, 1729–1791, 2710–2808 | 194 |
| `framework/languages/cpp/framework/src/runtime/channels/channel_outbound_exchange.cpp` | 1277–1309 | 33 |
| `framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.cpp` | 509–563, 1030–1060 | 86 |
| `framework/languages/cpp/framework/src/runtime/execution/serial_execution_queue.hpp` | 51–105, 223–253 | 86 |
| `framework/languages/cpp/framework/src/runtime/execution/state_lane.cpp` | 24–69 | 46 |
| `framework/languages/cpp/framework/src/runtime/messaging/request_failure_mapper.cpp` | 1–155, 206–243 | 193 |
| `framework/languages/cpp/framework/src/runtime/codecs/serializer.cpp` | 176–217 | 42 |
| `framework/languages/cpp/framework/src/runtime/dispatch/application_job_queue.hpp` | 78–121 | 44 |
| `framework/languages/cpp/framework/src/runtime/streams/stream_runtime.cpp` | 109–161 | 53 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp` | 603–619 | 17 |
| `framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp` | 174–188 | 15 |

### 구현 확인 — .NET

| 파일 | 읽은 행 | 읽은 행 수 |
|---|---|---:|
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkDrainAdmissionGate.cs` | 1–190 | 190 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs` | 1–240 | 240 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs` | 1–150 | 150 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkApplicationExecutionContext.cs` | 1–151 | 151 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs` | 1–76 | 76 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkStateLane.cs` | 62–75, 116–131 | 30 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs` | 406–422 | 17 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs` | 4968–5032, 10400–10442, 11335–11362, 12230–12305 | 212 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/FrameworkServiceTypes.cs` | 35–45 | 11 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/Wrappers/ZLinkBackendSpotNodeWrapper.cs` | 1525–1582 | 58 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkRequestFailureMapper.cs` | 1–196 | 196 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Streams/ZLinkSessionStreamCalls.cs` | 147–182 | 36 |
| `framework/languages/dotnet/src/Zlink.Framework.Contracts/Errors/ZLinkFrameworkException.cs` | 1–75 | 75 |

### 구현 확인 — .NET binding

| 파일 | 읽은 행 | 읽은 행 수 |
|---|---|---:|
| `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs` | 1495–1526 | 32 |

### 구현 확인 — Java

| 파일 | 읽은 행 | 읽은 행 수 |
|---|---|---:|
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/drain/ZLinkMeshDrainCoordinator.java` | 1–109 | 109 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java` | 285–335 | 51 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java` | 1–240 | 240 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkSerialExecutionQueue.java` | 30–119 | 90 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkExecutionLanePolicy.java` | 1–88 | 88 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java` | 4210–4320 | 111 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkFrameworkErrorOrigin.java` | 1–45 | 45 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/backend/ZLinkBackendRequestResult.java` | 62–103 | 42 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/handlers/ZLinkSuspendInvocationContext.java` | 98–114, 155–192 | 55 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelDirectCalls.java` | 382–401 | 20 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamSessionCalls.java` | 291–329 | 39 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/spots/ZLinkRelocationPayloadTransfer.java` | 258–286 | 29 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/execution/ZLinkStateLane.java` | 52–72, 102–117 | 37 |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/dispatch/ZLinkApplicationJobReceiveFlowController.java` | 105–150 | 46 |

### 구현 확인 — Node

| 파일 | 읽은 행 | 읽은 행 수 |
|---|---|---:|
| `framework/languages/node/packages/framework/src/runtime/application-jobs/application-ingress-record-owner.ts` | 1–216 | 216 |
| `framework/languages/node/packages/framework/src/runtime/foundation/operation-registry.ts` | 1–162 | 162 |
| `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-registry.ts` | 608–658 | 51 |
| `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts` | 135–164, 400–458, 504–652, 1080–1118, 1578–1620 | 320 |
| `framework/languages/node/packages/framework/src/runtime/foundation/operation-identity.ts` | 1–49 | 49 |
| `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts` | 350–415, 2375–2535, 2635–2652 | 245 |
| `framework/languages/node/packages/framework/src/runtime/execution/serial-execution-queue.ts` | 28–93, 489–534 | 112 |
| `framework/languages/node/packages/framework/src/runtime/execution/index.ts` | 186–233 | 48 |
| `framework/languages/node/packages/framework/src/runtime/execution/state-lane.ts` | 21–82 | 62 |
| `framework/languages/node/packages/framework/src/runtime/actors/actor-execution-context.ts` | 1–38 | 38 |
| `framework/languages/node/packages/framework/src/runtime/admission.ts` | 1–135 | 135 |
| `framework/languages/node/packages/framework/src/runtime/host/route-mesh-runtime.ts` | 443–489 | 47 |
| `framework/languages/node/packages/framework/src/runtime/framework-errors-internal.ts` | 42–83, 135–159 | 67 |
| `framework/languages/node/packages/framework/src/runtime/channels/channel-clients.ts` | 435–477 | 43 |
| `framework/languages/node/packages/framework/src/runtime/streams/session-calls.ts` | 168–221 | 54 |
| `framework/languages/node/packages/framework/src/runtime/streams/session-context.ts` | 293–322 | 30 |
| `framework/languages/node/packages/framework/src/runtime/application-jobs/receive-flow-controller.ts` | 1–125, 145–188 | 169 |
| `framework/languages/node/packages/framework/src/runtime/messaging/payload-codec.ts` | 291–324 | 34 |
| `framework/languages/node/packages/framework/src/contracts/Configuration/ConfigurationException.ts` | 1–6 | 6 |

### 구현 확인 — Kotlin

| 파일 | 읽은 행 | 읽은 행 수 |
|---|---|---:|
| `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/execution/ZLinkStateLane.kt` | 1–137 | 137 |
| `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkFrameworkExtensions.kt` | 1–145 | 145 |

### 검색만 수행한 범위와 생략

- `core/doc/spec/**/*.md`, `bindings/doc/spec/**/*.md`, `framework/doc/framework/common/spec/**/*.md`의 393개 Markdown 파일에서 glossary 링크를 전수 검색했다. 한국어 Framework common spec 144개로도 집계를 대조했다. 파일 수는 검색 분모이며 본문 완독 수가 아니다. 명시적 `<a id>`와 heading의 자동 slug를 함께 포함했고 glossary 자기 참조 및 fragment 없는 문서 링크는 항목별 incoming 링크에서 제외했다.
- F-R4-18에 열거한 18개 incoming 링크 중 위 본문 읽기 표에 없는 파일은 해당 링크 행만 확인했다. Runtime 검색 중 표 밖의 호출자·선언 매칭 행을 본 경우도 전체 파일 읽기로 계수하지 않았다. 예: `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:3911`, `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:118`, `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs:33`.
- 다른 작업이 반영한 인접 문서는 시작 HEAD와 재확인 HEAD의 diff로 읽었다. `03-mesh-node.ko.md`의 peer lifecycle 설명과 `04-actor-model.ko.md`의 durable replay 이동·정정은 인용 대상과 교차 확인했으며 R6 전체를 다시 검토한 것은 아니다.
- 요청의 `framework/languages/kotlin/**` 디렉터리는 존재하지 않는다. 실제 Kotlin 구현인 `framework/languages/java/zlink-framework-kotlin/**`의 state lane과 coroutine projection을 확인했다. 그 밖의 Kotlin 동작은 Java runtime 위임이 확인된 범위만 언급했다.
- 영어 스펙은 링크·키워드 검색 대상에 포함했지만 본문 parity 전체를 검토하지 않았다. C++·Java·Kotlin·Node의 native binding cancellation cleanup, 모든 codec cache, 모든 STREAM wrapper와 전체 queue 호출 경로는 완전 검증하지 않았다. 개별 finding에 확인하지 못한 언어·경로를 명시했다.
- Builds, tests, E2E, samples, benchmarks, site 렌더링 및 상태를 바꾸는 git 명령은 실행하지 않았다. 변경한 파일은 이 보고서 하나다.

## BLOCKERS

아래는 우선할 계약과 적용 범위를 supervisor가 결정해야 하는 질문이다. 답을 가정해 오류 종류·deadline·식별자 또는 public signature를 바꾸는 제안은 하지 않았다.

1. **Request timeout의 포함 구간:** `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:287`은 Global object request timeout에 Ready authority resolve·outbound admission·handler·reply 전부를 포함하지만 `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:393`과 `framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:234`의 일반 설명은 admission 이후 reply 대기로 설명한다. 일반 설명의 적용 범위에서 Global object를 명시적으로 제외하는 계약인가, 아니면 공통 request timeout 경계를 하나로 정해야 하는가?
2. **포화 즉시 `DeadlineExceeded`:** `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:155`, `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:283`, `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md:322`는 outbound waiter 자리가 없을 때의 즉시 결과를 deadline 계열로 정하지만 `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:35`·`framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:68`은 deadline 경과를 조건으로 둔다. 아직 deadline이 남은 overload에도 같은 kind를 쓰는 의도적인 operation 특례인가, 아니면 0.18.0의 오류 계약 변경 대상으로 분리할 사안인가?
3. **Store 실패의 오류 소유권:** Create·GetOrCreate의 Store resolve·reservation·commit 실패는 `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md:978`에서 `InternalFailure`이고, 사용할 수 없는 Store는 `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md:33`에서 `Unavailable`이다. Store가 일시적으로 unavailable인 creation 호출에서 어느 조항을 적용하며, creation 표의 ‘실패’에는 구체 kind로 이미 분류된 실패도 포함하는가?
4. **진행 중 호출 식별자의 의미:** `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md:389`·`framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md:393`은 언어별 길이 재량과 runtime 내 단일 형식을 허용·요구하고 `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md:413`은 이 식별자에 `OperationId`라는 이름을 금지한다. 반면 `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:328`은 service `OperationId` 128-bit와 별도의 `ReplyRouteId` 64-bit를 구별한다. Layering의 용어는 이 둘 중 무엇 또는 다른 내부 식별자를 뜻하는가?
5. **Binding 동기 `DONTWAIT` 예외:** `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:438`·`framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:447`은 제한적인 sync(+flags)·`DONTWAIT` 예외를 두지만 `bindings/doc/spec/async-coroutine-policy.ko.md:29`의 새 표면은 flags 없는 operation을 설명한다. 실제 .NET `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs:33`과 Framework `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:12248`에는 `TrySubmit` 사용 경로가 있다. R3와 대조할 때 이 예외를 유효한 binding 계약으로 남기는가, 아니면 public 경로 변경을 별도 0.18.0 항목으로 다루는가?
6. **Reply token 용어의 원래 범위:** `framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1911`·`framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md:1920`의 request cancellation·timeout 수명은 typed ClientServer reply에 한정된 정의인가? `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:248`의 STREAM reply token까지 같은 수명으로 의도한 것이라면 F-R4-16의 문서 범위 정정만으로 처리할 수 없으므로 어느 topology의 계약을 소유 정의로 둘 것인가?
