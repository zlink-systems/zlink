# Java L5~L7 production 진단·수정·검증

1단계 보고 시각: 2026-11-13T22:35:07.673069+00:00 UTC. 구현 전 main worktree는 `perf/4356-java-l57`, `3bd8756f99`, clean이다. 지정 ladder `cffa8009d3`는 `git archive`로 `/root/worktree/zlink-v21-s033/j57-scratch`에 새로 빌드했다. [build log](raw/build-scratch.log), [호출 대조](call-shape.md), 실행별 `fingerprint.json`에 실제 JAR 경로·SHA-256·HEAD·dirty를 보존했다.

## 원인

제거 가능한 source 비용은 **contended operation owner를 작업마다 기다리던 caller thread로 인계하는 경로**다. `Java/runtime/channels/ZLinkChannelSocketRegistry.java:609-619`가 외부 caller의 `nodeSubmission`에서 `owner.awaitTurn()`을 사용한다. `Java/runtime/internal/execution/ZLinkStateLane.java:130-137`은 작업 대신 `WaitingTurn`을 FIFO에 넣고, 앞 turn이 `:365-375`에서 caller를 깨우면 그 caller가 binding 제출을 실행한다. 8개 submit thread가 하나의 socket owner를 매번 인계한다. 기존 `ResultWork` drain은 여러 작업을 처리할 수 있지만 이 source 경로에서는 사용하지 않는다.

기본 C2 send profile에서 source `awaitTurn`의 grant 대기 wall 표본은 2,204개다(10ms 표본, [source wall](raw/baseline-L57-send/source-wall.collapsed)). 같은 경로에 `CompletableFuture.waitingGet → Signaller → Unsafe.park`, 다음 caller를 깨우는 `grantWaitingTurn → complete → unpark`가 나타난다. C2 compiler가 source CPU 표본 4,925개 중 3,834개를 차지하므로 전체 CPU 비율이나 처리량을 성능 판정에 사용하지 않는다([summary](raw/baseline-L57-send/summary.json)). 아래 C1 비교는 이 컴파일 간섭을 줄이기 위한 진단 조건이다.

Target에는 owner queue·dispatch bookkeeping과 fresh handler/scope·Spring 초기화의 고정 비용이 더해진다. 기본 JSON/base64 비용도 여전히 크다. Scope를 dispatch 사이에 공유하거나 completion dispatcher를 제거하는 수정은 제안하지 않는다. 현재 자료는 기존 28~52% 손실 전체를 하나의 비용으로 분해한 공식 성능 측정이 아니며, 21% gate 통과를 주장하지 않는다.

경로 약어: `Java`=`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/`, `Spring`=`framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/`, `.NET`=`framework/languages/dotnet/src/Zlink.Framework/`, `C++`=`framework/languages/cpp/framework/`. Java/Kotlin은 같은 JVM runtime을 사용한다.

## L5~L7 비용과 다른 언어의 처리

| 비용 | Java file:line | .NET file:line | C++ file:line | 다른 구현에서 덜 하는 일 |
|---|---|---|---|---|
| Source owner FIFO와 제출 | `runtime/channels/ZLinkChannelSocketRegistry.java:609`; `runtime/internal/execution/ZLinkStateLane.java:119,365` | `Runtime/Execution/ZLinkStateLane.cs:103,141,155,319` | `src/runtime/execution/state_lane.hpp:245,270,359`; `src/runtime/channels/channel_runtime.cpp:1498,1528` | .NET queued ResultWork와 C++ run/run_checked는 drain이 queued work를 실행한다. Java source는 별도의 caller-grant protocol을 선택한다. C++ run_sync 자체에는 같은 caller 인계가 있으나 두 경로를 구분한다. |
| Target owner queue/mailbox | `runtime/channels/ZLinkHandlerDispatch.java:16,45`; `execution/ZLinkSerialExecutionQueue.java:1700,1978,2047,2574` | `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:289,575,754` | `src/runtime/mesh/mesh_node_host_service.cpp:3870,3904` | .NET/C++는 owner drain에서 record를 계속 처리한다. Java generic serial queue는 record별 entry/result/gate와 completion observer를 가진다. |
| Handler activation·scope | `runtime/channels/ZLinkChannelHandlerInvoker.java:435,444`; `runtime/internal/handlers/ZLinkHandlerInstanceOwner.java:49,65,84` | `Runtime/Handlers/ZLinkHandlerDispatcher.cs:86,96,102,127` | `src/runtime/channels/route_handler_invoker.cpp:35,38,45,88` | 모두 invocation scope를 유지한다. .NET ValueTask와 C++ ready coroutine은 즉시 완료에 별도 CompletionStage chain이 필요하지 않다. |
| DI resolution·handler 초기화 | `runtime/internal/handlers/ZLinkHandlerActivator.java:37,395`; `Spring/ZLinkSpringHandlerFactory.java:457,485,492` | `Runtime/Handlers/ZLinkScopedHandlerInstanceOwner.cs:97,104`의 DispatchCatalog; `Runtime/Handlers/ZLinkHandlerDispatcher.cs:98-102` | `src/runtime/configuration/service_scope.hpp:38-56`; `src/runtime/channels/route_handler_invoker.cpp:38-45` | Java constructor metadata는 이미 준비한다. Spring은 handler마다 autowireBean/initializeBean을 수행한다. .NET/C++에는 Spring BeanWrapper/RootBeanDefinition 초기화가 없다. |
| Operation registry·deadline | `runtime/internal/service/ZLinkServiceOperationRegistry.java:327,341,516,617,698` | `Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:37,102,192,223` | `src/runtime/foundation/operation_registry.cpp:67,129,269,382` | terminal once와 deadline은 모두 필요하다. Java request는 entry+Operation과 deadline link를 보유한다. 즉시 수락 send는 등록을 우회한다. |
| Completion dispatch | `runtime/internal/service/ZLinkServiceCompletionDispatcher.java:49,101`; `runtime/internal/service/ZLinkServiceOperationRegistry.java:406` | `Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:12,18` | `src/runtime/dispatch/completion_turn.hpp:11`; `src/runtime/foundation/operation_registry.cpp:269` | .NET은 IThreadPoolWorkItem, C++는 completion turn을 직접 게시한다. Java는 Runnable을 commonPool에 게시한다. 독립 완료 turn은 세 구현 모두 유지한다. |
| Typed serializer lookup | `runtime/configuration/ZLinkCodecRegistration.java:243,337`; `runtime/channels/ZLinkChannelHandlerInvoker.java:422` | `Runtime/Codecs/ZLinkCodecRegistryBuilder.cs:72,77,139` | `include/zlink/framework/contracts/codecs/serializer.hpp:345-375` | 준비된 type/content-type 조회다. Java의 표본 비용은 owner 인계보다 작다. Payload serializer를 변경하지 않는다. |
| Thread 전환·reply | `runtime/binding/ZLinkJavaRawMeshNode.java:6269`; `execution/ZLinkSerialExecutionQueue.java:1978`; `runtime/channels/ZLinkMeshApplicationDispatcher.java:674` | `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:289`; `Runtime/Channels/ZLinkRouteHandlerInvoker.cs:123` | `src/runtime/mesh/mesh_node_host_service.cpp:3870`; `src/runtime/channels/route_handler_invoker.cpp:63-89` | 세 구현 모두 reply를 handler 완료 문맥에서 시작한다. Java source caller grant는 이 필수 target/completion 전환과 별도다. |

C++ `62e766f88d`의 L7은 protobuf codec을 끄고 typed JSON DTO를 production handler에 등록한다(`framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp:82-99`). Scope/owner/registry를 생략하지 않는다. Handler는 동기 void/DTO를 반환하므로 Java의 completedFuture를 만들지 않는다(`:24-42`). L4도 새 DTO body와 새 JSON reply를 encode한다(`framework/bench/grpc/cpp/ladder/ladder_wire.hpp:278-296`). C++의 L7/L4 증가를 필수 기능 생략이나 Java의 성능 하한 근거로 사용하지 않는다. 비교 근거는 [C++ call-shape](../l3868-s1/call-shape.md), [.NET call-shape](../lad-dotnet-s1/call-shape.md)다.

## 메시지당 profile

모든 source/target process와 시작한 보조 process는 CPU `6-7,14-15`에 고정했다. async-profiler 4.6 `ctimer=1ms`, `wall=10ms`, `alloc=256KiB`를 사용했다. 아래 네 셀은 `-XX:TieredStopAtLevel=1`, warmup 1초·active 2초이며 source/target의 errors=0, submitted=completed=received다. Run마다 process 수명은 `lifecycle.json`, thread affinity/policy 원본은 `start.json`, `active.json`에 있다. 19초 timeout과 18초 runner 상한을 사용한다.

CPU는 compiler/GC/VM thread 표본을 제외한 추정 µs/message다. Allocation은 phase당 고정 SourceMetrics/latency 배열을 제외한 sampled B/message다. Wall은 Framework/bench/binding 호출 stack에서 관측한 thread 시간을 합산한 µs/message이며 왕복 latency가 아니다. 변환 구간은 active 시작부터 source 종료·target drain을 확인한 snapshot까지다. 짧은 JVM 실행과 동시 머신 부하의 영향을 받으므로 모양과 제거되는 비용을 비교한다.

| 패턴·단계(1K) | 역할 | 메시지 수 | CPU µs/msg | 할당 B/msg | wall µs/msg | 근거 |
|---|---|---:|---:|---:|---:|---|
| send · L4 | source | 50,294 | 29.98 | 4803 | 350.34 | [raw](raw/baseline-c1-L4-send/costs-full.json) |
| send · L4 | target | 50,294 | 39.41 | 12385 | 43.35 | [raw](raw/baseline-c1-L4-send/costs-full.json) |
| send · L5~L7 | source | 30,214 | 46.37 | 4885 | 590.12 | [raw](raw/c1-before-L57-send/costs-full.json) |
| send · L5~L7 | target | 30,214 | 92.94 | 15383 | 127.42 | [raw](raw/c1-before-L57-send/costs-full.json) |
| backpressure · L4 | source | 18,333 | 108.11 | 18203 | 1468.94 | [raw](raw/c1-before-L4-bp/costs-full.json) |
| backpressure · L4 | target | 18,333 | 110.02 | 16873 | 147.82 | [raw](raw/c1-before-L4-bp/costs-full.json) |
| backpressure · L5~L7 | source | 4,566 | 137.98 | 20094 | 4332.02 | [raw](raw/valid-before-L57-bp/costs-full.json) |
| backpressure · L5~L7 | target | 4,566 | 194.70 | 19463 | 814.72 | [raw](raw/valid-before-L57-bp/costs-full.json) |

C1 L5~L7 exclusive cost 분류는 각 stack에서 가장 안쪽의 관련 모듈을 한 번만 계산한다. Raw `*-cpu-full.collapsed`, `*-wall-full.collapsed`, `*-alloc-full.collapsed`와 분류 원본 `costs-full.json`을 보존했다. 작은 항목의 allocation 표본 부재는 무할당을 뜻하지 않는다.

| 항목 | send source CPU / B | send target CPU / B | bp source CPU / B | bp target CPU / B |
|---|---:|---:|---:|---:|
| Owner queue/mailbox | 16.85 / 1397 | 33.99 / 2716 | 12.26 / 1263 | 67.46 / 4363 |
| Handler activation/scope | 표본 없음 | 4.60 / 434 | 표본 없음 | 9.86 / 230 |
| DI resolution | 표본 없음 | 12.71 / 980 | 표본 없음 | 15.33 / 1320 |
| Operation registry | 표본 없음 | 표본 없음 | 24.53 / 2009 | 표본 없음 |
| Completion dispatch | 표본 없음 | 표본 없음 | 1.53 / 0 | 표본 없음 |
| Typed serializer lookup | 0.17 / 0 | 0.56 / 0 | 0.44 / 0 | 0.88 / 0 |
| Typed JSON | 7.74 / 1952 | 22.37 / 10212 | 28.47 / 11081 | 47.09 / 11368 |
| Thread scheduling | 11.12 / 26 | 3.44 / 0 | 21.68 / 172 | 4.16 / 0 |

## 결정 소유와 수정안

Spec root는 `framework/doc/framework/common/spec/server/`다.

| 결정 | 소유 조항 | Java / Kotlin 현재 판정 위치 | .NET 현재 판정 위치 | C++ 현재 판정 위치 | Node 현재 판정 위치 | 모을 소유자 |
|---|---|---|---|---|---|---|
| Source operation turn FIFO·반환 전 첫 제출 | `01-execution/06-state-ownership-and-lanes.ko.md:230`; `01-execution/08-messaging-hot-path.ko.md:68-100` | `ZLinkChannelSocketRegistry.java:609-619`, `ZLinkJavaRawMeshNode.java:598-608`, `ZLinkServiceOperationRegistry.java:270-287`; `ZLinkStateLane.java:119-147,365-375` | `Runtime/Execution/ZLinkStateLane.cs:103-155,319-334` | `src/runtime/execution/state_lane.hpp:245-280,359`; `src/runtime/channels/channel_runtime.cpp:1498-1528` | `runtime/execution/state-lane.ts:54-80` | 기존 Java StateLane ResultWork FIFO drain |
| Handler 준비의 실행 자원·scope 수명 | `01-execution/08-messaging-hot-path.ko.md:249-287`; `00-foundation/06-framework-api.ko.md` §10·11 | `ZLinkChannelHandlerInvoker.java:444-457`; `ZLinkHandlerInstanceOwner.java:49-84` | `Runtime/Handlers/ZLinkHandlerDispatcher.cs:96-136` | `src/runtime/channels/route_handler_invoker.cpp:38-89` | `runtime/channels/channel-dispatchers.ts:381,560` | 각 기존 activation owner, Java awaitTurn 유지 |
| Request terminal once·독립 completion | `01-execution/01-submit-and-completion.ko.md` §10·11; `08-messaging-hot-path.ko.md` E3/E5 | `ZLinkServiceOperationRegistry.java:406-423,516`; `ZLinkServiceCompletionDispatcher.java:49,101` | `Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:192-223`; `ZLinkCompletionDispatcher.cs:12` | `src/runtime/foundation/operation_registry.cpp:129,269` | `runtime/foundation/operation-registry.ts:240,332` | 기존 registry와 completion dispatcher |

| 대안 | 판정 |
|---|---|
| awaitTurn을 async 게시로 바꾸고 바로 반환 | 첫 binding 시도/registration의 반환 전 완료 보장을 바꾸므로 기각한다. |
| 기존 ResultWork drain의 결과를 source caller가 반환 전에 기다림 | 채택안이다. 기존 FIFO·예외·close·deadline을 유지하고 caller별 실행 권한 인계를 제거한다. |
| awaitTurn 전체의 caller-thread 실행을 바꿈 | DI 준비의 실행 자원 경계를 바꿀 수 있으므로 기각한다. |

**Production 수정안(B: 기존 내부 비용 결함).** 내부 `ZLinkStateLane.awaitResult`를 synchronous bridge로 추가하고, 이미 존재하는 ResultWork enqueue/drain을 재사용한다. 무경합은 기존 inline claim으로 실행해 private future를 만들지 않는다. 기존 submit의 ResultWork 생성·enqueue를 helper 한 곳으로 옮겨 새 bridge와 공유한다. `ZLinkChannelSocketRegistry.nodeSubmission`, `ZLinkJavaRawMeshNode.inSourceTurn`, `ZLinkServiceOperationRegistry.submitOwnerTurn`의 외부 source 제출에서 이 bridge를 사용한다. Runtime 실행 문맥의 기존 async 경로와 DI용 awaitTurn은 유지한다.

소유 계층: Framework source operation owner(StateLane). 변경 분류: B. 네 언어 모두 FIFO·registration-before-submit·terminal once를 유지한다. 바꾸는 것은 spec §6이 허용한 내부 실행 자원 배치이며 wire·public Framework API·spec 변경은 필요하지 않다. StateLane은 JPMS public export 대상이 아닌 내부 package다.

규칙 수(판정 protocol 수): **source queued work 실행 2(caller grant / ResultWork drain) → 1(ResultWork drain)**. StateLane의 FIFO/claim 소유자 1→1. 새 state·flag·queue·lock·timer는 없다. DI caller-turn protocol은 해당 준비 경계에서 유지한다. ResultWork enqueue helper는 기존 코드를 이동해 한 곳에 둔다.

## 구현 전 검증 계획

- Source binding 시도가 submit 반환 전에 끝나는지, FIFO·state lane 비재진입·close·실패 전달을 검사한다.
- Contended source work가 기존 drain 한 회전에서 처리되는 회귀 검사를 원래 source 경로에서 먼저 실패시킨다.
- Handler scope lifetime·cleanup once·operation completion once 관련 기존 framework test를 실행한다.
- Java format check와 touched 경로 unit/fake-backend test를 실행한다.
- 같은 scratch bench/binding에서 수정한 core만 교체해 두 패턴을 다시 profile한다. CPU/allocations와 source grant/worker/completion stack을 전후 대조한다.

이번 profile은 measure.lock을 사용하지 않았다. 다른 worktree와 그 산출물, GitHub, push, PR, internal-docs, sub-agent를 사용하지 않았다. 시작 제한/종료 제한이나 thread snapshot race로 완료하지 못한 준비 실행도 `raw/`에 모두 보존했으며 위 수치에는 포함하지 않았다.



## 구현 결과

Framework source operation owner의 queued work를 기존 ResultWork drain으로 실행하도록 수정했다. Source caller는 private result를 기다리므로 registration과 첫 binding 시도는 submit 반환 전에 끝난다. 무경합 호출에는 private future를 만들지 않는다. 기존 async source 경로와 handler/DI용 caller-thread awaitTurn은 유지한다.

| 변경 파일 | 결과 |
|---|---|
| `runtime/internal/execution/ZLinkStateLane.java:144,185` | 내부 awaitResult bridge와 기존 ResultWork enqueue의 단일 소유 helper |
| `runtime/channels/ZLinkChannelSocketRegistry.java:615` | RouteMesh node/spot source 제출에 같은 bridge 사용 |
| `runtime/binding/ZLinkJavaRawMeshNode.java:603` | 직접 source transport 제출도 같은 owner drain 사용 |
| `runtime/internal/service/ZLinkServiceOperationRegistry.java:282` | 직접 operation 등록·제출의 반환 전 경계에 같은 bridge 사용 |
| `src/test/.../channels/ZLinkNodeSubmitTurnTest.java:334` | 8 queued public send의 FIFO, 반환 전 첫 binding, 완료 한 번, owner drain 실행 검증 |
| `src/test/.../internal/execution/ZLinkStateLaneTest.java:50` | 무경합 무할당, 예외 identity, 비재진입, owner context 복원, close 후 거부 검증 |

수정 전 production 코드에 새 source 회귀 검사를 먼저 실행했다. Binding을 실행한 thread 수가 기대값 1에 대해 실제 8이어서 실패했다([원본 XML](raw/regression-before.xml)). 같은 검사가 수정 후 통과했다. 기존 caller-thread awaitTurn 검사는 expectation을 바꾸지 않고 통과했다.

### Source owner 원인 분리

전체 request 경로의 JSON·native I/O·completion 비용과 source 인계를 구분하기 위해 같은 JVM·같은 StateLane 구현에서 기존 awaitTurn과 새 awaitResult를 caller/result/result/caller 순서로 교대했다. 8 caller가 같은 owner의 짧은 상태 변경을 수행하며 owner context·총 실행 수 일치를 검사했다. C1 고정, 동일 profiler 설정, 각 구간 1초이며 전체 process는 19초 timeout 안에서 정상 종료했다. Framework end-to-end 처리량 비교에 사용하지 않는다.

| owner 경계 | process CPU µs/turn 범위 | sampled allocation B/turn 범위 | 근거 |
|---|---:|---:|---|
| 기존 caller grant | 9.90–14.02 | 76.28–81.31 | [window별 원본·collapsed 집계](raw/owner-probe/summary.json) |
| 기존 ResultWork drain 재사용 | 1.40–1.49 | 6.03–9.29 | 같은 파일 |

Core transport나 payload codec을 바꾸지 않은 이 대조에서 source 인계 비용이 줄었다. ResultWork drain이 밀린 work를 처리해 queued caller 수가 줄고 기존 무경합 무할당 경로도 다시 사용한다. 입력/실행 수와 source context가 일치한 원본은 [windows.json](raw/owner-probe/windows.json), 도구는 [OwnerProbe.java](tools/OwnerProbe.java)다. 이 수치는 전체 L5~L7 비용이나 21% budget의 개선률이 아니다.

### 전체 production 경로 전후

수정 후에는 원래 scratch bench·binding JAR을 유지하고 새로 빌드한 scratch core만 classpath 앞에 두었다. 네 production edit 외의 baseline 차이를 섞지 않았다. [freshness·hash](raw/fixed-core-freshness.json), [fixed build](raw/build-scratch-fixed.log), 실행별 `fingerprint.json`이 근거다. Source/target 모두 CPU 6–7,14–15, C1, warmup 1초·active 2초다.

CPU는 compiler/GC/VM thread를 제외한 sampled µs/message, allocation은 B/message다. Source owner wall은 **기존 grant 대기와 새 result 대기를 모두 같은 owner 항목으로 합산**했다. 최종 분류는 `costs-symmetric.json`이다. 초기 파생 분류 `costs-full.json`도 보존했으며 초기 after owner-wall 값은 새 result 대기를 thread scheduling으로 분류했으므로 최종 대조에 사용하지 않는다.

| 1K 패턴·역할 | CPU µs/msg 전 → 후 | allocation B/msg 전 → 후 | source owner wall µs/msg 전 → 후 | 전 / 후 raw |
|---|---:|---:|---:|---|
| send-saturation · source | 46.37 → 41.03 | 4885 → 4840 | 501.42 → 288.00 | [전](raw/c1-before-L57-send/costs-symmetric.json) / [후](raw/c1-after-L57-send/costs-symmetric.json) |
| send-saturation · target | 92.94 → 62.59 | 15383 → 13825 | — | [전](raw/c1-before-L57-send/costs-symmetric.json) / [후](raw/c1-after-L57-send/costs-symmetric.json) |
| request-backpressure · source | 137.98 → 145.86 | 20094 → 23708 | 1979.85 → 653.06 | [전](raw/valid-before-L57-bp/costs-symmetric.json) / [후](raw/c1-after-L57-bp/costs-symmetric.json) |
| request-backpressure · target | 194.70 → 152.95 | 19463 → 20179 | — | [전](raw/valid-before-L57-bp/costs-symmetric.json) / [후](raw/c1-after-L57-bp/costs-symmetric.json) |

Send source의 CPU·할당과 두 패턴의 owner 대기는 줄었다. **Backpressure source 전체 CPU·할당은 감소하지 않았다.** 증가한 char[]·byte[] 표본 대부분은 기존 reply JSON `TextBuffer` 경로에 있다(`source-alloc-full.collapsed`의 `Entry.dispatch → ZLinkJsonMessageSerializer.decodeJson → UTF8StreamJsonParser → TextBuffer`). 이 profile만으로 그 차이를 이번 bridge의 전체 allocation 개선으로 해석하지 않는다. Target은 수정하지 않았는데도 CPU 추정치가 변하므로 짧은 실행의 전체 비용 개선률을 확정하지 않는다. Source 인계 제거 자체는 동일 JVM 대조와 회귀 검사로 확인했다.

| 수정 후 실행 | submitted=completed=received | errors | 수명·affinity 근거 |
|---|---:|---:|---|
| send 1K | 44,653 | 0 | [lifecycle](raw/c1-after-L57-send/lifecycle.json), [start](raw/c1-after-L57-send/start.json), [active](raw/c1-after-L57-send/active.json) |
| backpressure 1K | 13,965 | 0 | [lifecycle](raw/c1-after-L57-bp/lifecycle.json), [start](raw/c1-after-L57-bp/start.json), [active](raw/c1-after-L57-bp/active.json) |

### Thread 인계 수

Framework가 추가하는 실행 자원 전환을 message의 호출 경로에서 센다. Core/binding 내부 I/O thread는 그 계층의 소유이므로 별도다. Bench는 외부 submit thread에서 호출한다.

| message 경로 | 수정 전 | 수정 후 |
|---|---|---|
| Source encode(E1) | caller에서 실행, 전환 0 | 동일 |
| Source owner turn(E2/E3/E4) | 무경합 0. 경합 시 grant를 1회 기다린 뒤 **각 caller**가 자기 thread에서 실행한다. 작업 사이에 owner 권한을 계속 인계한다. | 무경합 0. queued work는 기존 owner drain 자원으로 전환 1회, caller는 private result를 기다린다. 기존 최대 100건 drain 재사용. |
| Target receive → owner queue | transport pump에서 직접 receiver 호출, 전환 0 | 동일 |
| Target queue → handler worker | record당 실행 자원 전환 1회. worker task는 여러 record를 drain한다. | 동일 |
| Handler → activation/DI → reply submit | 같은 handler 실행 자원, 추가 전환 0 | 동일 |
| Binding terminal → source completion dispatcher(E5) | request/지연 send 1회, 즉시 admitted send 0 | 동일 |
| Typed reply decode → caller callback | 같은 E5 turn, 추가 전환 0 | 동일 |

이번 수정은 source caller별 권한 인계를 기존 drain 처리로 모은다. 한 message의 source owner 실행 자원 전환은 경합 시 0에서 1로 늘 수 있으나, 실행 권한을 매번 다음 caller에게 넘겨야 진행하던 protocol을 제거한다. Controlled 8건 검사의 binding 실행 자원은 8개 caller에서 drain 1개로 바뀌었다. 새 queue·worker pool·supervisor·타이머를 추가하지 않았다.

### 검증과 남은 항목

| 검사 | 결과 | raw |
|---|---|---|
| 새 회귀 검사·변경 전 | expected owner drain 1, actual caller threads 8로 실패 | [log](raw/regression-before.log), [XML](raw/regression-before.xml) |
| Focused StateLane·source submit·registry·handler owner | 114 tests, 실패/오류/skip 0 | [집계](raw/focused-after-counts.json), [log](raw/focused-after.log) |
| 전체 Java core test | **19,962 tests, 470 suites, 실패/오류/skip 0** | [집계](raw/framework-final-counts.json), [log](raw/framework-final.log), [XML](raw/framework-final-xml/) |
| fakeBackendTest | NO-SOURCE, 실행 대상 0 | 같은 log |
| Java 전체 format check | PASS(수정 파일 외 변경 없음) | [log](raw/format-final.log) |
| git diff --check | PASS | 최종 검증 metadata |

전체 core suite에는 scope lifetime을 검사하는 `ZLinkMeshApplicationDispatcherTest.sameNamespaceStartsNextBeforePriorTerminal`, `ZLinkHandlerInstanceOwnerTest`와 registry의 reply/cancel/close/timeout terminal-once 검사가 포함된다. 기존 assertion을 완화하지 않았다. 새 source 검사는 metadata FIFO와 반환 전 binding·완료 한 번을 함께 확인한다.

- 소유 계층: Framework source operation owner(StateLane).
- Spec: `06-state-ownership-and-lanes` 「반환 전 완료 보장」, `08-messaging-hot-path` E2–E4. Handler 준비/DI 경계와 E5는 기존 소유자가 유지한다.
- 교차언어: .NET ResultWork와 C++ 정상 run/run_checked의 queued drain을 대조했다. Java/Kotlin의 public 결과·FIFO·deadline·terminal once 의미를 바꾸지 않는다. Node는 기존 FIFO promise/terminal owner를 유지한다.
- 분류: B(내부 실행 비용 결함). Spec·wire·public Framework API 변경 없음.
- 규칙 수: source queued work 실행 protocol **2→1**, state owner **1→1**. 새 state/flag/queue/lock/timer 0.
- 리팩토링 점검: source protocol 분리 1건을 수정하고 ResultWork enqueue 중복을 기존 코드 이동으로 방지했다. 넘긴 결함 0, 추가 execution-gate 동기화 0.

남은 항목은 공식 동일 조건에서의 L4/L5~L7 21% budget 재판정이다. 이번 작업에서는 지시대로 measure.lock을 사용하지 않았고 처리량 판정을 하지 않았다. Target generic queue bookkeeping, Spring handler 초기화와 JSON 비용은 profile에 보존했다. 이 비용을 scope 공유·serializer 등록·timeout 증가로 우회하지 않았다. Core unit test와 format의 남은 실패는 없다.
