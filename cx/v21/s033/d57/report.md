# .NET L5~L7 production 진단과 수정안

1단계 보고: 2026-11-13T23:07:55.103941+00:00 UTC. 구현 전 branch `perf/4356-dotnet-l57`, HEAD/base `3bd8756f99`, clean. 지정 ladder `29d9fca82b`를 `d57-scratch`에 archive하여 새로 빌드했다. [호출 대조](call-shape.md), [build](raw/build-client-l57-before.log), [server build](raw/build-server-l57-before.log).

## 원인과 Java 대조

**.NET의 추가 비용은 target의 record별 mailbox·runtime admission·새 handler scope와 그 사이의 임시 표현에 집중한다. Java가 지적한 source caller-grant protocol과는 다르다.** .NET source는 이미 `ZLinkStateLane`의 ResultWork FIFO drain을 사용한다. Java 보고서를 다시 읽고 [사본](raw/java-report-before-proposal.md)을 보존했다. Java의 `nodeSubmission → awaitTurn → WaitingTurn` 인계는 .NET에 없다.

1K send의 target managed allocation은 L4 1,747 → L5~L7 3,966 B/message다. L5~L7에서 ExecutionContext와 AsyncLocal map, queued record, backend route, DI scope와 invocation value boxing이 추가된다. request에는 operation/deadline·native reply observation·독립 completion이 더해진다. Scope를 공유하거나 completion을 inline으로 바꾸는 것은 수정안이 아니다.

제거 가능한 production 비용은 다음 세 가지다.

1. `Runtime/Handlers/ZLinkHandlerMethodInvoker.cs:60-72`가 handler 결과를 object로 boxing하고, 준비된 awaiter 결과인 `ValueTask<object?>`도 object로 boxing한다. `ZLinkHandlerInvocationEngine.cs:62-63,80-81,96-97`은 이를 다시 해석한다. send/request 모두 allocation trace에 두 종류의 ValueTask box가 있다. 선언 반환형은 준비 때 이미 알려져 있으므로 이 runtime 표현 변환은 필요 없다.
2. `Runtime/Execution/ZLinkStateLane.cs:207-220`의 동기 `TryPost`는 lane이 비어 있어도 PostWork와 mailbox를 거친다. 같은 클래스의 `RunAsync:103-155`에는 이미 FIFO를 보존하는 무경합 inline 경로가 있다. Target mailbox enqueue(`Service/ZLinkMeshOwnedMailbox.cs:80`)와 runtime lease release(`Host/ZLinkFrameworkRuntime.cs:1830`)가 record마다 이 차이를 지불한다. AsyncLocal 소유 표시는 유지하되 queued wrapper와 별도 동기 진입 절차를 없앨 수 있다.
3. `Service/FrameworkServiceTypes.cs:719-804`의 batch는 큰 `MeshReceiveRecord`를 포함한 tuple을 읽어 parts/payload owner 한 field를 바꾼 뒤 전체 tuple을 다시 쓴다. Claim→dispatch 중 같은 record 표현을 반복 복사한다. 소유 batch의 기존 entry에서 해당 field만 이전하면 된다. 추가 DTO나 owner를 만들 이유가 없다.

이 자료로 원래 throughput 손실 21~39% 전부의 기여율이나 21% budget 통과를 판정하지 않는다. 정해진 CPU에서 20초 이하로 수행한 진단이며 다른 job의 실행/guard 상태와 profiler 비용이 포함된다.

## L5~L7 비용과 언어별 경로

경로: `.NET`=`framework/languages/dotnet/src/Zlink.Framework/`, `Java`=`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/`, `Node`=`framework/languages/node/packages/framework/src/`. Java/Kotlin은 같은 JVM runtime이다.

| 비용 | .NET file:line | Java equivalent | Node equivalent · 덜 하는 일 | Java와 같은 원인인가 |
|---|---|---|---|---|
| Source owner turn | `Runtime/Execution/ZLinkStateLane.cs:103,155,319`; `Runtime/Service/ZLinkManagedMeshNode.cs:11618` | `runtime/channels/ZLinkChannelSocketRegistry.java:603-619`, `runtime/internal/execution/ZLinkStateLane.java:119,365` | `runtime/execution/state-lane.ts:54-80`; event loop, OS caller 깨우기 없음 | **아니다.** Java 보고서의 caller-grant 인계가 .NET에는 없다. |
| Target queue/mailbox·claim | `Runtime/Service/ZLinkMeshOwnedMailbox.cs:74-126`, `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:289,485,615` | `runtime/channels/ZLinkHandlerDispatch.java:16,45`; `execution/ZLinkSerialExecutionQueue.java:1978,2047,2574` | `runtime/backend/mesh-dispatch-pump.ts:114`; 한 event loop에서 record와 결과를 직접 처리하고 OS worker 전환이 없다 | bookkeeping 비용은 공통. .NET의 idle PostWork·큰 value tuple 복사는 별도다. |
| Handler activation·scope·DI | `Runtime/Handlers/ZLinkHandlerDispatcher.cs:86-136`, `ZLinkScopedHandlerInstanceOwner.cs:97-127` | `runtime/channels/ZLinkChannelHandlerInvoker.java:435-457`; `runtime/internal/handlers/ZLinkHandlerInstanceOwner.java:49-84` | `runtime/handlers/handler-instance-scope.ts:60-86,154-213`; 새 scope 수명은 유지한다. Spring 초기화나 Microsoft DI scope dictionary는 없다 | fresh scope는 공통. Java Spring autowire/initialize 비용과는 다르다. |
| Handler awaitable·async state machine | `Runtime/Handlers/ZLinkHandlerMethodInvoker.cs:60-72`, `ZLinkHandlerInvocationEngine.cs:62-97` | `runtime/handlers/ZLinkHandlerMethodInvoker.java:189-207`; CompletionStage를 그대로 돌려줌 | `runtime/channels/channel-dispatch-pipeline.ts:100-130,157-187,335-356`; 동기 결과는 그대로 사용하고 비동기 결과만 continuation 추가 | **아니다.** .NET의 ValueTask 2중 boxing/재해석이다. Java에는 per-message CompletionStage chain 비용이 별도로 있다. |
| Operation registry·deadline | `Runtime/Service/ZLinkManagedMeshNode.cs:11590-11705,14272`; `Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:37,192` | `runtime/internal/service/ZLinkServiceOperationRegistry.java:327,341,516,617,698` | `runtime/foundation/operation-registry.ts:240,332`; map과 indexed deadline, 즉시 send는 등록 생략 | 기능 비용은 공통. Java source grant의 원인과는 별개다. |
| Completion dispatch | `Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:12-18`; `Runtime/Service/ZLinkManagedMeshNode.cs:14304` | `runtime/internal/service/ZLinkServiceCompletionDispatcher.java:49,101` | `runtime/foundation/operation-registry.ts:332`; 같은 event loop의 새 turn, OS thread handoff 없음 | 독립 completion은 공통 계약이다. 제거하지 않는다. |
| Typed serializer lookup | `Runtime/Codecs/ZLinkCodecRegistryBuilder.cs:72-84,139`; `Runtime/Channels/ZLinkRouteHandlerInvoker.cs:55` | `runtime/configuration/ZLinkCodecRegistration.java:243,337` | `runtime/channels/channel-dispatch-pipeline.ts:349`, `channel-envelope.ts`; 준비된 message type·codec 조회 | 두 보고서 모두 lookup이 주원인이라는 증거가 없다. |
| ThreadPool·completion observation | `Runtime/Service/ZLinkManagedMeshNode.cs:11179-11221,11638`; `Runtime/Execution/ZLinkProcessExecutionPool.cs:68,289` | `runtime/binding/ZLinkJavaRawMeshNode.java:6269`, `execution/ZLinkSerialExecutionQueue.java:1978` | `runtime/channels/channel-dispatch-pipeline.ts:118-130,175-187`; 동기 handler의 별도 task 게시 없음 | 둘 다 고정비가 있지만 Java caller-grant와는 다르다. .NET 동기 ValueTask 계층 자체는 Task나 hop을 만들지 않는다. |

## Profile 방법과 메시지당 비용

모든 시작 process는 taskset CPU `6-7,14-15`, source/target 각각 19초 watchdog이다. Warmup 2초·active 5초, payload 1K, source 8 submit thread·window 100. 각 run의 manifest에 binary SHA-256, base, dirty, 시작/종료 시각을 보존했다. source/target start·mid-active thread affinity도 raw에 있다. `measure.lock`을 사용하지 않았다.

- EventPipe: dotnet-trace 8.0.547301, cpu-sampling + CLR GC allocation ticks + threading, 역할별 6초. Allocation은 active timestamp 안의 AllocationAmount64를 합한 sampled B/message다. 큰 고정 latency array를 포함한 process 전체 값이며, category는 가장 안쪽 관련 호출을 한 번만 계산한다.
- OS CPU: `/proc/<pid>/stat`의 process CPU ticks 차이/message. 별도로 perf cpu-clock 199Hz, DWARF 16K, `--clockid mono`를 사용했다. CPU split은 perf sample period/message이며 전체 CPU보다 작을 수 있다(attach·trace 구간 경계, unwind·sampling coverage). unresolved/native/GC/JIT를 Framework에 임의로 배분하지 않는다.
- 아래 비교는 `DOTNET_TieredCompilation=0`으로 짧은 실행의 background tier-up 간섭을 줄인 진단이다. 앞선 default-tiered profile도 raw에 보존했다. EventPipe의 Managed 표본 개수는 on-CPU 시간이 아니므로 CPU 시간으로 환산하지 않는다. 최초 perf default clock은 monotonic_raw여서 interval match가 실패했고, 명시적 mono로 다시 수집했다. 원본은 보존했다.

| 패턴·단계 | 역할 | 완료 message | process CPU µs/message | sampled B/message | 근거 |
|---|---|---:|---:|---:|---|
| L4-request-backpressure | source | 88,826 | 128.68 | 8052 | [L4-request-backpressure/source](raw/before-notier/L4-request-backpressure/costs.json) |
| L4-request-backpressure | target | 88,826 | 65.18 | 3858 | [L4-request-backpressure/target](raw/before-notier/L4-request-backpressure/costs.json) |
| L4-send-saturation | source | 364,956 | 26.93 | 4462 | [L4-send-saturation/source](raw/before-notier/L4-send-saturation/costs.json) |
| L4-send-saturation | target | 364,956 | 13.43 | 1747 | [L4-send-saturation/target](raw/before-notier/L4-send-saturation/costs.json) |
| L5_7-request-backpressure | source | 51,013 | 220.92 | 10910 | [L5_7-request-backpressure/source](raw/before-notier/L5_7-request-backpressure/costs.json) |
| L5_7-request-backpressure | target | 51,013 | 113.11 | 6230 | [L5_7-request-backpressure/target](raw/before-notier/L5_7-request-backpressure/costs.json) |
| L5_7-send-saturation | source | 192,338 | 37.75 | 4473 | [L5_7-send-saturation/source](raw/before-notier/L5_7-send-saturation/costs.json) |
| L5_7-send-saturation | target | 192,338 | 35.98 | 3966 | [L5_7-send-saturation/target](raw/before-notier/L5_7-send-saturation/costs.json) |

아래는 perf CPU µs/message / allocation B/message다. 각 행의 근거는 위 셀의 costs.json과 source/target cpu-stacks.json·profile.json이다. 표본 0은 무비용을 뜻하지 않는다.

| 셀 | 비용 | source CPU / B | target CPU / B |
|---|---|---:|---:|
| L4-request-backpressure | owner queue/mailbox | 0.00 / 0 | 0.34 / 102 |
| L4-request-backpressure | activation/DI scope | 0.00 / 0 | 0.00 / 0 |
| L4-request-backpressure | handler invocation | 0.00 / 0 | 0.00 / 0 |
| L4-request-backpressure | operation registry/deadline | 0.00 / 2 | 0.06 / 0 |
| L4-request-backpressure | completion dispatcher | 0.00 / 0 | 0.00 / 0 |
| L4-request-backpressure | serializer lookup | 0.11 / 11 | 0.23 / 23 |
| L4-request-backpressure | runtime/permit scope | 0.00 / 0 | 0.00 / 0 |
| L4-request-backpressure | worker/threadpool | 0.00 / 5 | 0.00 / 0 |
| L4-request-backpressure | typed JSON | 2.94 / 3366 | 1.07 / 3054 |
| L4-request-backpressure | other Framework | 1.64 / 290 | 4.41 / 409 |
| L4-request-backpressure | binding/Core | 24.33 / 207 | 26.93 / 175 |
| L4-request-backpressure | runtime/native/unresolved | 36.09 / 9 | 23.14 / 10 |
| L4-send-saturation | owner queue/mailbox | 0.00 / 0 | 0.33 / 192 |
| L4-send-saturation | activation/DI scope | 0.00 / 0 | 0.00 / 0 |
| L4-send-saturation | handler invocation | 0.00 / 0 | 0.00 / 0 |
| L4-send-saturation | operation registry/deadline | 0.00 / 0 | 0.00 / 0 |
| L4-send-saturation | completion dispatcher | 0.00 / 0 | 0.00 / 0 |
| L4-send-saturation | serializer lookup | 0.08 / 4 | 0.17 / 0 |
| L4-send-saturation | runtime/permit scope | 0.00 / 0 | 0.00 / 0 |
| L4-send-saturation | worker/threadpool | 0.00 / 3 | 0.00 / 0 |
| L4-send-saturation | typed JSON | 0.30 / 1220 | 0.32 / 1369 |
| L4-send-saturation | other Framework | 1.13 / 897 | 0.52 / 72 |
| L4-send-saturation | binding/Core | 9.40 / 83 | 4.14 / 74 |
| L4-send-saturation | runtime/native/unresolved | 8.19 / 2 | 5.70 / 2 |
| L5_7-request-backpressure | owner queue/mailbox | 0.20 / 180 | 1.28 / 654 |
| L5_7-request-backpressure | activation/DI scope | 0.00 / 0 | 0.39 / 84 |
| L5_7-request-backpressure | handler invocation | 0.00 / 0 | 0.59 / 100 |
| L5_7-request-backpressure | operation registry/deadline | 2.36 / 1533 | 0.00 / 0 |
| L5_7-request-backpressure | completion dispatcher | 0.00 / 0 | 0.00 / 0 |
| L5_7-request-backpressure | serializer lookup | 0.10 / 4 | 0.20 / 31 |
| L5_7-request-backpressure | runtime/permit scope | 0.00 / 0 | 1.48 / 528 |
| L5_7-request-backpressure | worker/threadpool | 0.30 / 11 | 0.30 / 0 |
| L5_7-request-backpressure | typed JSON | 2.27 / 2980 | 0.89 / 2885 |
| L5_7-request-backpressure | other Framework | 3.84 / 1565 | 12.02 / 1570 |
| L5_7-request-backpressure | binding/Core | 34.97 / 557 | 38.42 / 359 |
| L5_7-request-backpressure | runtime/native/unresolved | 71.32 / 19 | 38.42 / 17 |
| L5_7-send-saturation | owner queue/mailbox | 0.00 / 0 | 1.02 / 667 |
| L5_7-send-saturation | activation/DI scope | 0.00 / 0 | 0.10 / 111 |
| L5_7-send-saturation | handler invocation | 0.00 / 0 | 0.39 / 107 |
| L5_7-send-saturation | operation registry/deadline | 0.00 / 0 | 0.03 / 0 |
| L5_7-send-saturation | completion dispatcher | 0.00 / 0 | 0.00 / 0 |
| L5_7-send-saturation | serializer lookup | 0.00 / 2 | 0.10 / 0 |
| L5_7-send-saturation | runtime/permit scope | 0.00 / 0 | 0.63 / 498 |
| L5_7-send-saturation | worker/threadpool | 0.00 / 7 | 0.26 / 0 |
| L5_7-send-saturation | typed JSON | 0.13 / 1191 | 0.26 / 1340 |
| L5_7-send-saturation | other Framework | 0.03 / 47 | 3.79 / 1016 |
| L5_7-send-saturation | binding/Core | 5.49 / 874 | 6.92 / 187 |
| L5_7-send-saturation | runtime/native/unresolved | 7.47 / 6 | 15.60 / 3 |

### 실행 자원 인계 수

| 경로 | Framework 인계 수/message | 근거와 제한 |
|---|---|---|
| Source send 즉시 admission | 정상 무경합 0 | E1~E4가 caller에서 끝난다. Binding 내부 thread는 제외한다. BACKPRESSURED만 dispatcher 게시 1회가 추가된다. `SendDirectWireAsync:10973-11016`은 result 기반 EnsureAcceptedAsync를 이미 사용한다. |
| Source request 제출 | 0~2 | selector·operation 두 state lane은 무경합 inline, 경합이면 기존 drain 자원으로 이전한다. Java식 caller-grant는 없다. `RequestNativeDurableWireAsync:11618-11704`. |
| Target owner receive→application worker | 정상 1 | 수신 batch를 owner queue에 두고 공유 worker가 claim한다. 이후 동기 activation/handler/reply는 같은 worker에서 끝나며 Task.Run을 추가하지 않는다. `ZLinkMeshDispatchPump:289,485,575`. |
| Source reply→caller continuation | Framework dispatcher 게시 1 | `PendingOperation.DispatchReply:14304`→`ZLinkCompletionDispatcher.Post:12`. State lane의 추가 reply 왕복은 없다. Binding의 자체 completion thread는 별도다. |
| Node 동기 handler | OS 자원 전환 0 | 같은 event loop에서 invoke·reply. 비동기 handler는 Promise continuation을 추가한다. |

이는 해당 호출 경로의 handoff 수이며, profile만으로 모든 메시지의 동적 thread ID 연쇄를 추적했다고 주장하지 않는다. run의 `results.json`에는 실제 process ThreadPool completed work item 수도 보존했다. 이는 batch·HTTP·binding work도 포함하므로 handoff와 같지 않다. 후속 검증에서 기존 completion isolation test의 실행 자원 전환 검사를 사용한다.

## 결정 소유와 production 수정 제안

Spec root: `framework/doc/framework/common/spec/server/`.

| 결정 | 소유 spec | .NET 현재 판정 위치 | Java/Kotlin | C++ | Node | 수정 후 소유자 |
|---|---|---|---|---|---|---|
| 동기 state turn 실행·FIFO·비재진입 | `01-execution/06-state-ownership-and-lanes.ko.md:171-239`; `08-messaging-hot-path.ko.md` §3·6 | `ZLinkStateLane.cs:103-155,207-220` | `ZLinkStateLane.java:119,365` | `src/runtime/execution/state_lane.hpp:245,359,468` | `runtime/execution/state-lane.ts:54-89` | 기존 .NET lane의 한 동기 claim/inline/enqueue 경로 |
| 준비된 handler 결과 해석·scope lifetime | `01-execution/08-messaging-hot-path.ko.md:249-287`; `00-foundation/06-framework-api.ko.md:490-494,539` | `ZLinkHandlerMethodInvoker.cs:60-72`; `ZLinkHandlerResultAwaiter.cs:12-53`; `InvocationEngine.cs:62-97`; `Dispatcher.cs:96-136` | `runtime/handlers/ZLinkHandlerMethodInvoker.java:189-207`; `ZLinkHandlerInstanceOwner.java:49-84` | `src/runtime/channels/route_handler_invoker.cpp:38-89` | `channel-dispatch-pipeline.ts:335-356`; `handler-instance-scope.ts:60-86` | 등록 시 return type에 맞춰 만든 invoker 한 곳. Scope owner는 기존 dispatcher 유지 |
| record parts/payload 단독 이전 | `01-execution/05-payload-ownership-and-codec.ko.md` §2; `08-messaging-hot-path.ko.md` W1~W4 | `FrameworkServiceTypes.cs:732-804`; `ZLinkMeshDispatchPump.cs:709-750` | `runtime/channels/ZLinkHandlerDispatch.java:45` | `src/runtime/mesh/mesh_node_host_service.cpp:3870` | `runtime/backend/mesh-dispatch-pump.ts` claim state | 기존 batch entry. 새 record/queue/owner 없음 |
| terminal once와 completion 독립 | `01-execution/01-submit-and-completion.ko.md` §10·11 | `ZLinkMeshCompletionTable.cs:192-242`; `PendingOperation:14304`; `CompletionDispatcher.cs:12` | `ZLinkServiceOperationRegistry.java:406-423,516` | `src/runtime/foundation/operation_registry.cpp:129,269` | `operation-registry.ts:332` | 변경 없음 |

| 대안 | 판정 |
|---|---|
| scope 공유, worker 직렬화, completion dispatcher 생략 | fresh scope·독립 handler·completion 계약을 바꾸므로 기각한다. |
| timeout/queue budget 증가, bench handler 특수 처리 | 원인을 고치지 않으므로 기각한다. |
| 기존 owner primitive의 동기 경로와 준비된 typed invoker 재사용, batch에서 단독 이전 | 채택한다. 공개 동작·wire·scope·deadline·completion 경쟁을 유지한다. |

**수정안 B — 기존 내부 비용 결함.**

1. 내부 invoker가 `ValueTask<object?>`를 직접 반환한다. Expression compiler는 실제 반환형 그대로 typed await helper를 호출한다. object 기반 awaitable cache/재분류와 outer ValueTask box를 삭제한다. 실제 값 타입 reply의 object 표현, cancelable token의 boxing은 별도 비용으로 남긴다.
2. 동기 RunAsync와 TryPost의 claim/inline/enqueue 구현을 한 곳으로 모은다. Post가 lane을 즉시 claim하고 선행 work가 없으면 기존 context 보존·복원 규칙으로 action을 실행한다. 경합·자기 lane post는 기존 FIFO PostWork이고, async work는 기존 scheduler를 유지한다. Post의 예외 게시·close 의미를 유지한다.
3. MeshReceiveBatch의 parts/payload owner 이전은 CollectionsMarshal의 ref entry로 해당 field만 갱신한다. public/API 변경이나 다른 소유자의 storage 접근은 없다. Ref는 동기 method 안에서만 쓰고 await를 넘기지 않는다.

규칙 수(결정 위치): 동기 claim·inline·enqueue **2→1**, handler 반환형 해석 **2→1**, batch ownership 이전 위치 **1→1**. 합계 **5→3**. 새 state·flag·queue·lock·timer를 추가하지 않는다. 기존 lane의 FIFO와 AsyncLocal current는 그대로 하나다. 단순화는 복제된 진입 절차와 동적 결과 표현을 없애는 데 있다.

교차언어 public 동작은 동일하다. Java source 실행 자원 개선과 .NET 내부 value boxing 제거는 언어별 runtime 비용 결함이며 새로운 공통 계약을 정하는 D가 아니다. Spec·wire·public API 변경이 필요하지 않으므로 사용자 지시의 “먼저 보고, 이후 구현” 순서대로 아래 구현을 진행한다.

## 검증 예정

- Allocation 회귀를 원본 production에서 먼저 실패시키고 수정 후 통과시킨다. 동기/비동기·성공/예외/cancel·IValueTaskSource single consumption을 검사한다.
- State lane의 FIFO·비재진입·post 예외·close·suppressed ExecutionContext와 multi-thread contention을 검사한다.
- Batch parts/payload owner 이전·cleanup once, 기존 MeshNodeRouteDispatcher의 scope isolation/lifetime·독립 실행, completion terminal once/isolation을 검사한다.
- .NET 전체 format check와 관련 Framework unit tests를 실행한다. 수정한 production 소스만 scratch에 복사하여 rebuild·fingerprint 뒤 같은 두 패턴을 다시 profile한다.


## 구현 결과와 전후 대조

Production은 다음 다섯 파일에서 바꿨다. Benchmark·wire·serializer·scope sharing 정책은 바꾸지 않았다. 이 장은 원인·제안을 기록한 뒤 구현한 결과이며, 최초 보고서는 [phase1 사본](raw/report-phase1.md)에 보존했다.

| production 파일 | 변경 |
|---|---|
| `Runtime/Handlers/ZLinkHandlerMethodInvoker.cs` | 내부 invoker 반환형을 ValueTask<object?>로 고정하고 compiled expression에서 실제 반환형을 typed helper에 직접 전달한다. |
| `Runtime/Handlers/ZLinkHandlerResultAwaiter.cs` | 선언 반환형 판정을 준비 시점 한 곳으로 모았다. object awaiter cache와 runtime awaitable 재분류를 삭제했다. non-generic null Task, object로 선언된 Task 결과, fault/cancel 결과를 유지한다. |
| `Runtime/Handlers/ZLinkHandlerInvocationEngine.cs` | 준비된 invoker 결과를 그대로 반환/await한다. Normalized ValueTask를 다시 object로 만들지 않는다. Permit 반환 시점은 유지한다. |
| `Runtime/Execution/ZLinkStateLane.cs` | RunAsync와 TryPost가 같은 RunSynchronous의 claim/inline/enqueue를 사용한다. 빈 lane의 post는 PostWork/mailbox allocation이 없다. 자기 lane post와 경합은 기존 FIFO, async work는 기존 scheduler다. |
| `Runtime/Service/FrameworkServiceTypes.cs` | 소유 batch의 ref entry에서 parts/payload owner field만 이전한다. 큰 record를 포함한 tuple 전체 read-modify-write를 없앴다. |

Test 파일은 `HandlerResultAwaiterTests`, `StateLaneTests`, `MeshMailboxReadinessTests`에 검사를 추가했고, 내부 typed delegate를 사용하는 다섯 기존 test 파일의 stub 반환형을 맞췄다. 기존 ordering·scope·completion assertion은 완화하지 않았다.

### Allocation 회귀의 양방향 검증

[수정 전](raw/test-baseline-regressions.log) 새 검사 6개가 실제 실패했다. [수정 후 focused](raw/test-focused-second.log)에서는 이 6개와 ValueTask source의 sync/async success·fault·cancel, 결과 1회 소비, 선언형 유지, FIFO·self-post·context 복원·단독 이전을 포함한 **66개가 통과**했다.

| 경계 | 수정 전 B/call | 수정 후 | 근거 |
|---|---:|---:|---|
| 동기 reference reply | 40 | 0 | PreparedInvocation_DoesNotBoxAwaitableResults |
| completed Task<object> | 40 | 0 | 같은 검사 |
| completed ValueTask<object> | 80 | 0 | 같은 검사 |
| void | 40 | 0 | 같은 검사 |
| completed ValueTask | 72 | 0 | 같은 검사 |
| 빈 lane의 동기 post | 104 | 기존 inline result와 같은 이하의 storage | IdlePost_UsesTheSameTurnStorageAsAnInlineResult. 원본 inline result는 72 B/turn이다. |

이 결과는 hot path에서 실제 생성하던 box와 work item을 제거했음을 보인다. 동기 호출 결과가 즉시 완료되는 경우의 측정이며, 비동기 I/O에 필요한 결과 객체까지 무할당이라는 뜻은 아니다.

### 실제 production profile

아래의 CPU는 process 전체 active/drain CPU ticks/message이고, allocation은 EventPipe sampled B/message다. Source/target은 각 run에서 errors=0, submitted=completed=received다. 두 snapshot에서 전 thread의 CPU affinity가 6-7,14-15의 부분집합이고 scheduling policy가 SCHED_OTHER였다([validation](raw/profile-validation.json), [policy audit](raw/policy-audit.json)). 전후 configuration은 no-tier, warmup 2초·active 5초다.

| 1K 셀 · 역할 | CPU µs/message 전→후 | sampled B/message 전→후 | raw |
|---|---:|---:|---|
| send-saturation · source | 37.75 → 44.86 | 4473 → 4373 | [전](raw/before-notier/L5_7-send-saturation/costs.json) / [후](raw/after-notier/L5_7-send-saturation/costs.json) |
| send-saturation · target | 35.98 → 43.39 | 3966 → 3421 | [전](raw/before-notier/L5_7-send-saturation/costs.json) / [후](raw/after-notier/L5_7-send-saturation/costs.json) |
| request-backpressure · source | 220.92 → 168.94 | 10910 → 10610 | [전](raw/before-notier/L5_7-request-backpressure/costs.json) / [후](raw/after-notier-3/L5_7-request-backpressure/costs.json) |
| request-backpressure · target | 113.11 → 89.64 | 6230 → 5770 | [전](raw/before-notier/L5_7-request-backpressure/costs.json) / [후](raw/after-notier-3/L5_7-request-backpressure/costs.json) |

**전체 CPU 개선률은 확정하지 않는다.** Send CPU 총량은 증가했고, 수정하지 않은 source의 backpressure CPU도 크게 변했다. Side CPU의 동시 부하·sampling·runtime 관리 비용이 섞인 짧은 실행에서 이 변화를 수정의 인과 효과로 단정할 수 없다. 제거한 ValueTask boxing stack이 후속 profile에 없다는 것과 아래 실제 allocation 감소를 판정 근거로 사용한다. Thread·native runtime·binding 비용을 코드 변경의 이득으로 임의 배분하지 않았다.

EventPipe tick의 attribution은 표본이므로 process가 자체 기록한 allocation counter와 따로 대조했다. 아래 값은 `results.json`의 active phase delta/message와 target final stats/message다. 후자는 마지막 drain·stats 준비도 포함한다. 최종 source/target count를 분모로 사용했고, 그 원본은 위 validation에 연결했다.

| 1K 패턴 | source active B/msg 전→후 | target active B/msg 전→후 | target final B/msg 전→후 |
|---|---:|---:|---:|
| send-saturation | 4471.5 → 4508.3 | 4175.2 → 3994.9 | 4191.9 → 4012.0 |
| request-backpressure | 11373.3 → 11379.5 | 6551.3 → 6370.4 | 6583.8 → 6404.0 |

Target의 실제 할당은 두 패턴 모두 **약 180 B/message 감소**했다(send 4.3%, backpressure 2.8%). Source 실제 allocation은 감소하지 않았다. Scope·record·native payload·JSON·request completion 자체의 할당은 남아 있으며 이 감소를 전체 ladder 손실의 해소나 21% budget 통과로 표현하지 않는다.

Profile collector의 rundown이 실행 상한을 넘긴 두 backpressure 시도는 raw에 보존하고 위 비교에서 제외했다(`after-notier/L5_7-request-backpressure`, `after-notier-retry`). target/source process의 19초 watchdog이나 전체 run 상한을 늘리지 않았다. `after-notier-3`은 16.12초에 완료했다. Data snapshot을 collector 종료 대기보다 먼저 저장하도록 진단 도구 순서만 바꿨다.

### Java 최종 보고서 대조

최종 보고서를 다시 읽고 [사본](raw/java-report-before-final.md)을 보존했다. Java local commit은 `da04022584`다. Java는 기존 caller-grant를 queued ResultWork drain으로 모았고, controlled 8-thread owner probe의 CPU가 9.90–14.02 → 1.40–1.49 µs/turn으로 줄었다. 반면 전체 backpressure source CPU·allocation 감소는 확인하지 못했다고 보고했다. 이 Java 수치를 .NET의 성능 하한이나 이번 변경의 이득으로 사용하지 않는다.

**같은 원인인가: source caller 인계는 아니다.** .NET은 원래 ResultWork drain이다. Target queue·fresh scope·JSON의 고정 비용은 공통이지만, 이번 .NET 변경의 ValueTask boxing/재해석과 idle PostWork·value tuple 복사는 .NET 내부 표현의 문제다. 두 언어 모두 scope와 독립 completion을 유지했고 공식 21% 재판정은 하지 않았다.

### 소유·분류·리팩토링

- 소유 계층: Framework state lane, 준비된 handler invoker, 소유 receive batch. Core·binding의 연결·재시도·completion 선택을 추가로 판정하지 않는다.
- Spec: `06-state-ownership-and-lanes` §5 FIFO·비재진입·반환 전 경계, `08-messaging-hot-path` §4.3의 prepared activation·동기 결과 경로, `05-payload-ownership-and-codec` §2, `06-framework-api` §10·11의 fresh scope·terminal cleanup.
- 교차언어: Java/Kotlin의 caller-grant 비용과 구분했다. C++의 typed ready result와 Node의 synchronous result/같은 event loop 처리를 대조했다. 공개 결과·순서·scope·deadline·terminal once의 언어 차이를 만들지 않았다.
- 변경 분류: **B — 기존 내부 비용 결함.** Spec·wire·public API 변경 0.
- 규칙 수: 동기 lane 진입 2→1, 반환형 해석 2→1, batch 소유 1→1, 합계 **5→3**. 새 persistent state/flag/queue/lock/timer 0. 기존 awaiter cache 하나를 삭제했다.
- 리팩토링 점검: 발견 3건, 수정 3건, 이월 결함 0. 새 execution-gate 우회 동기화 0. Scope 수명과 C1 terminal owner는 기존 소유자를 유지했다.

남은 성능 검증은 예약된 동일 조건의 공식 L4/L5~L7 21% budget 재판정이다. 지시대로 이번 작업에서 measure.lock을 사용하거나 throughput을 판정하지 않았다.


### 추가 context 검토와 수정안

공유 inline 경로를 검토하면서 flow suppression에서의 context 복원 차이를 확인했다. 기존 TryPost의 async drain은 compiler의 method builder가 caller ExecutionContext 전체를 복원하지만, RunAsync의 수동 fallback은 Capture가 null이면 lane marker만 복원한다. 이 fallback을 공유하면 post callback의 다른 AsyncLocal 변경이 caller에 남을 수 있다. 기존 scope/ambient 계약을 유지하기 위해 suppressed/normal × success/failure 회귀를 추가한다.

수정안은 기존 inline turn 본문을 동기 완료하는 ValueTask method로 옮기고 .NET 기본 method builder의 context 복원을 사용하는 것이다. Inline turn은 완료된 ValueTask만 통과하므로 suspension·Task allocation·thread hop을 추가하지 않는다. Claim과 drain release는 같은 기존 소유자에 남기며, inline context를 복원한 다음 queued drain을 시작한다. 수동 Capture/Restore·suppression 분기를 삭제한다. 소유 spec은 기존 StateLane §5, 변경 분류 B다. 새 state/flag/queue/lock/timer나 public API는 없다.
