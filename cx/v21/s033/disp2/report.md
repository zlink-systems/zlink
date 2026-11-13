# #1639 C++ Framework completion dispatcher 결정 소유 표

근거 입력은 `m-result.md` §M5(줄 1–4), `design.md` §3(줄 1–14)와 §6 F3(§6 표 F3)다. M5는 completion worker 8개, 샘플의 54%가 pthread 조건변수 sleep/wake 경로, worker 합계 약 27 µs/msg를 기록한다. 본 문서는 측정 원인을 소스와 구분한다.

## C++ 경로와 결정 소유

Binding 완료는 `raw_route_port_t::request_impl`의 finish callback으로 들어와(`framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:364-378`), task 반환형에서는 `task_shared_state_t::complete`를 부른다(`raw_route_port.cpp:324-339`). Framework async request는 binding의 `.async()` stage를 관찰하고 terminal 결과를 finish로 전달한다(`framework/languages/cpp/framework/src/runtime/backend/raw_binding_adapter.hpp:146-198`). 등록된 operation은 completion dispatcher borrow를 등록 시 저장하고(`framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:382-410`), terminal을 mutex 밖에서 꺼낸 뒤 `post_completions`가 각 항목을 `dispatch_completion(..., false, owner, dispatcher)`에 넘긴다(`operation_registry.cpp:539-549, 552-565, 269-295`). dispatcher는 조건에 맞을 때만 inline 실행하고, 그 외에는 저장된 dispatcher에 `post`하여 새 turn scope에서 callback을 실행한다(`framework/languages/cpp/framework/src/runtime/dispatch/provider_task_context.cpp:162-191`). Worker는 `io_context.run()`을 호출한다(`provider_task_context.cpp:56-65`). Callback의 task 완료는 `task_shared_state_t::complete`가 mutex 아래 terminal을 한 번 고정하고, mutex를 놓은 뒤 waiter·terminal callback을 전달한다(`framework/languages/cpp/framework/include/zlink/framework/contracts/dispatch/task.hpp:622-659`). 대기 등록의 완료 전달은 등록된 scheduler가 있으면 이를 사용한다(`task.hpp:720-788`).

| 결정 | 지금 결정하는 모든 곳(file:line, 현재 규칙) | 옮길 단일 소유자 |
|---|---|---|
| (a) pool 크기·thread 수 | `completion_dispatcher_t`가 최소 2와 process-affinity CPU 수의 최댓값으로 worker 수를 정하고 각 worker를 생성한다(`provider_task_context.cpp:53-65`). Linux는 process affinity, Windows는 process affinity mask, 나머지는 hardware concurrency를 반환한다(`runtime/utils/available_processor_count.hpp:19-34`). M5의 8은 측정 환경에서 선택된 개수다(측정 출처: `m-result.md` §M5 줄 1). | `completion_dispatcher_t`의 worker 정책 한 곳. processor 수 조회는 값 제공자다. |
| (b) completion 게시 executor | 등록 시 operation이 dispatcher를 보관한다(`operation_registry.cpp:382-410`). `borrow_completion_dispatcher`는 completion scope, runtime hook 또는 process dispatcher를 선택한다(`provider_task_context.cpp:134-149`). `dispatch_completion`은 inline 조건을 판정하고 dispatcher의 `post`를 호출한다(`provider_task_context.cpp:174-191`). | dispatcher가 게시 대상과 실행-turn 정책을 함께 소유. Runtime hook은 기존 명시적 scheduler 계약으로 남는다(`provider_task_context.cpp:134-149, 203-207`). |
| (c) idle wake와 wake당 item 수 | worker의 `io_context.run()`은 `do_run_one`을 반복한다(`provider_task_context.cpp:56-65`; Boost `scheduler.ipp:218-224`). `post`는 즉시 completion을 큐에 넣고 `wake_one_thread_and_unlock`을 부른다(Boost `scheduler.ipp:357-378`). 큐가 비면 worker는 `wakeup_event_.wait()`에서 대기한다(Boost `scheduler.ipp:511-525`). `do_run_one`은 한 handler를 실행하고 반환하므로, run 반복이 큐를 다시 확인한다(Boost `scheduler.ipp:460-508`). | `io_context` scheduler가 wake/실행을 소유하고 dispatcher가 `run()` 수명을 소유한다. Application 차원의 추가 drain 소유자는 두지 않는다. |
| (d) item 순서·독점성 | `post_completions`는 completion chain 순서대로 항목을 각각 게시한다(`operation_registry.cpp:269-295`). 여러 worker가 동시에 `run()`하므로 독립 callback의 전역 FIFO나 callback별 단독 worker 보장은 없다(`provider_task_context.cpp:56-65`; 독립 진행 test: `test_cpp_framework_operation_registry.cpp:77-128`). Registry의 terminal은 mutex 아래 한 번 take한다(`operation_registry.cpp:539-549`). Task 상태도 첫 terminal만 수락하고 등록 continuation을 순서대로 전달한다(`task.hpp:622-659`). | terminal 승자는 operation registry, task 상태 변경과 waiter 전달은 `task_shared_state_t`; completion callback의 배치·turn만 dispatcher. 이 구분은 결정마다 한 소유자다. |
| (e) 대기 continuation 신호 | M5의 `completion_signal_t`는 Framework 구현이 아니라 bench의 완료 슬롯 신호다. task 관찰 callback이 슬롯을 넣고 `notify_one`한다(`framework/bench/grpc/cpp/common/bench_async.hpp:36-60`); 벤치가 신호를 기다린다(`bench_async.hpp:69-73`, `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:911-924`). Framework task는 `complete`가 result를 publish하고 대기 continuation을 delivery한다(`task.hpp:622-659`). | Framework continuation은 `task_shared_state_t` 한 곳. Bench signal은 bench 대기 프로토콜의 별도 consumer이며 dispatcher 실행 결정이 아니다. |

**설정과 소비자.** C++ Framework completion pool에는 사용자 option 또는 환경변수 분기가 없다. 기본값은 `max(2, available_processor_count())`이며, processor count 입력은 OS process affinity 또는 fallback hardware concurrency다(`provider_task_context.cpp:58-65`; `available_processor_count.hpp:19-34`). 실제 worker 소비자는 공유 `io_context.run()` 호출뿐이다(`provider_task_context.cpp:56-65`). Dispatcher 게시 호출자는 operation registry(`operation_registry.cpp:281, 695`; 동일 함수 `post_completions` 호출은 `:378, :682, :772, :820`), state lane(`runtime/execution/state_lane.hpp:420-443`; `state_lane.cpp:95-97`), stateful waiter(`runtime/stateful/stateful_object_runtime.cpp:96`), actor transfer coordinator(`runtime/spots/actor_transfer_coordinator.cpp:135`)다. `completion_dispatcher_worker_count`는 테스트 관찰용이며(`completion_turn.hpp:10-12`), 관련 검증은 dispatcher owner 및 operation registry test다(`test_cpp_framework_completion_dispatcher_owner.cpp:16-117`; `test_cpp_framework_operation_registry.cpp:235-311`).

### Boost.Asio wake 근거

`io_context.run()`의 반복은 한 번 호출에서 여러 ready handler를 처리한다(`scheduler.ipp:218-224`); 각 `do_run_one`은 handler 한 개 실행 후 반환한다(`scheduler.ipp:460-508`). 따라서 같은 worker가 handler 뒤에 run 반복으로 돌아왔을 때 큐에 ready 항목이 남아 있으면 다시 잠들기 전 여러 항목을 처리할 수 있다. 이는 한 callback을 묶어 직렬 drain하는 동작이 아니라 각각 별도 Asio operation 실행이다.

`post_immediate_completion`은 operation을 큐에 넣고 `wake_one_thread_and_unlock`을 호출한다(`scheduler.ipp:357-378`). 해당 함수는 waiter가 없으면 unlock하며, waiter가 있으면 signal one을 한다(`scheduler.ipp:674-685`). POSIX event는 waiter 수가 있을 때 pthread condition signal을 실행하고(`posix_event.hpp:64-74, 88-100`), 실제 대기는 `pthread_cond_wait`다(`posix_event.hpp:112-122`). conditionally-enabled event는 mutex가 enabled일 때 이 동작을 위임한다(`conditionally_enabled_event.hpp:48-100`).

**가설:** M5의 32k completion/s를 8 worker가 고르게 처리한다고 놓으면 worker당 평균 4k/s, 즉 completion 사이 약 250 µs다. callback은 worker를 약 3.37 µs/msg 사용하므로 대부분의 시간 worker는 빈 큐에서 대기한다. 완료가 빈 큐 상태를 깨우면 item당 wake가 반복되어 sleep/wake 표본 비중 54%를 설명할 수 있다. 실제 각 완료가 새 wake였는지는 M5의 source trace만으로 확정할 수 없다. Asio는 여러 ready 항목이 이미 큐에 있을 때 한 번 깨어난 worker가 이를 연속 처리한다.

## .NET·Java 대응 표

| 결정 | .NET (file:line, 현재 규칙) | Java (file:line, 현재 규칙) |
|---|---|---|
| (a) pool 크기·thread 수 | Framework가 별도 크기를 정하지 않고 `ThreadPool.UnsafeQueueUserWorkItem`을 사용한다(`framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:13-17`). 소비·worker 수는 .NET ThreadPool runtime 소유이며 Framework option/env 설정은 이 dispatcher에 없다(`ZLinkCompletionDispatcher.cs:6-17`). | `ForkJoinPool.commonPool()`을 사용한다(`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java:51-54`). 크기는 JDK common pool 정책 및 JVM 설정 소유이며 Framework 자체 pool/thread 설정은 없다(같은 파일 `:7, 51-54`). |
| (b) 게시 executor | `ZLinkCompletionDispatcher.Post`가 process ThreadPool로 게시한다(`ZLinkCompletionDispatcher.cs:13-17`). Mesh completion table이 게시한다(`ZLinkMeshCompletionTable.cs:195, 204, 219, 241`). Mesh node·deadline index도 같은 dispatcher를 사용한다(`ZLinkManagedMeshNode.cs:14300, 14320`; `ZLinkRequestDeadlineIndex.cs:215, 311, 319`). | Dispatcher가 common pool로 게시한다(`ZLinkServiceCompletionDispatcher.java:51-54`). Service operation registry는 terminal을 post하고(`ZLinkServiceOperationRegistry.java:378-423`), request stage의 admission view도 deliver를 사용한다(`runtime/internal/calls/ZLinkRequestStages.java:79-83`). |
| (c) idle wake와 wake당 item 수 | ThreadPool work item 하나를 queue한다(`ZLinkCompletionDispatcher.cs:13-17, 19-35`). wake/worker가 한 번 깨어난 뒤 처리하는 item 수는 Framework가 정하지 않고 ThreadPool runtime 소유다. | common pool에 개별 Runnable을 execute한다(`ZLinkServiceCompletionDispatcher.java:51-54`). wake/worker item 수는 ForkJoinPool 소유다. `postChain`은 각 WorkItem을 각각 post한다(`:56-76`). |
| (d) 순서·독점성 | WorkItem 하나가 `Execute`와 accounting `Completed`를 같은 ThreadPool work item에서 수행한다(`ZLinkCompletionDispatcher.cs:19-59`). 별도 전역 FIFO·독점 worker를 Framework가 보장하지 않는다. | 각각의 `WorkItem.run`이 개별 dispatch와 registration release를 한다(`ZLinkServiceCompletionDispatcher.java:91-124`). chain은 개별 post이고(`:56-76`), 독립 callback의 전역 FIFO·전용 worker 보장은 없다. |
| (e) continuation 신호 | `PendingCompletion`은 dispatcher `WorkItem`이며 completion table이 이를 게시한다(`ZLinkMeshCompletionTable.cs:272`; dispatcher `:19-59`). .NET caller continuation은 Framework dispatcher가 지정하지 않고 Task/ThreadPool 런타임 정책을 따른다. | dispatcher delivery가 `caller.complete` 또는 `completeExceptionally`를 실행한다(`ZLinkServiceCompletionDispatcher.java:126-148`). CompletionStage 호출자는 그 완료를 관찰한다. |

## S01 §11 새 execution turn

요청된 원문(`framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:490-513`):

> ## 11. 완료 callback의 execution turn (구현)
>
> **완료를 확정할 때 잡은 잠금 안에서 application callback을 실행하지 않는다.**
> Callback이 다시 runtime을 호출할 때 같은 잠금을 요구하면 교착이 되기 때문이다.
> Timer 취소와 payload 정리도 그 바깥에서 한다.
>
> 잠금을 놓은 직후 같은 호출 stack에서 callback을 바로 실행하는 것만으로는 충분하지
> 않다. 그렇게 하면 transport의 응답 처리나 timeout 처리가 끝나기 전에 application
> code가 runtime에 다시 진입할 수 있다. 완료 callback은 process가 공유하는 completion
> dispatcher에 넣고, 아래에서 정한 새 execution turn 조건에 따라 실행한다.
>
> 순서는 이렇다.
>
> 1. 완료 권한을 확정한다.
> 2. 잠금을 놓는다.
> 3. callback을 dispatcher에 넣는다.
> 4. 새 execution turn에서 callback을 실행한다.
>
> **새 execution turn은 다른 thread를 뜻하지 않는다.** 같은 실행 자원에서 callback을 실행할 때는 완료
> 확정·게시 처리의 호출 stack과 lane-current scope가 반환된 뒤 다음 turn에서 시작한다. 다른 실행 자원에서
> 실행할 때는 완료 판정의 잠금과 lane-current scope를 callback에 넘기지 않고 독립된 turn에서 시작하며,
> 다른 자원의 상위 호출 stack 전체가 반환됐다는 전역 순서까지 요구하지 않는다. 그 turn을 어느 실행 자원이
> 실행하는지는 [Messaging hot path 「4.4」](08-messaging-hot-path.ko.md#44-실행-자원-배치--수신-역할-인계)가
> 정한다.

측정 가능한 조건은 (1) 완료 확정 잠금을 callback이 잡지 않음, (2) 같은 실행 자원에서는 completer의 publish 호출 stack 및 lane-current scope가 반환된 다음 callback 시작, (3) 별도 자원에서는 completer 잠금·lane scope를 넘기지 않은 별도 turn, (4) caller가 runtime 밖에서 대기했어도 동일한 callback turn 조건이다(위 §11 줄 492-517). 다른 thread 사용은 요구하지 않으며 다른 자원의 상위 stack 전체 반환도 요구하지 않는다(§11 줄 508-517). 서로 다른 completion은 여러 ready item을 한 worker가 처리하더라도 각 callback마다 별도 turn이어야 하고, 먼저 실행된 callback의 return에 나머지 callback 진행이 매여서는 안 된다(S08 §4.4 표: `08-messaging-hot-path.ko.md:322-326`).

S08 §4.4 제약 원문 위치와 적용: 기존 queue·gate·claim 순서와 completion 권한을 유지한다(`08-messaging-hot-path.ko.md:323`); 무관한 completion callback을 하나의 callback 반환에 의존하는 drain으로 합치지 않는다(`:324`); 실행 자원을 새로 만들거나 상한을 늘리지 않는다(`:326`). 따라서 Asio `run()`의 반복으로 ready handler를 연속 실행하는 것은 각 handler의 독립성을 유지하는 한 허용된다. 단일 callback 내부에서 여러 completion을 차례로 실행하는 drain은 허용되지 않는다.

기존 C++ 검증은 infrastructure completion의 새 turn과 두 번째 pool hop 부재를 확인한다(`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_outside_runtime_resumption.cpp:234-256, 269-278`). 별도 completion이 첫 callback의 return을 기다리지 않고 진행하고 각 terminal이 한 번 전달되는지도 확인한다(`test_cpp_framework_operation_registry.cpp:77-128`). **한 번의 OS wake로 복수 ready item이 실행됐는지를 검증하는 테스트는 찾지 못했다.**

## 변경 옵션과 권고

판정 위치 수는 표의 해당 결정에서 실제로 판단하는 코드 위치 수, 규칙 수는 같은 결정을 내리는 규칙 위치 수로 센다. 공유 dispatcher 설계는 실행 결정이 `dispatch_completion`·registry의 게시 호출·pool 생성부에 나뉘어 있다(각 위치: `provider_task_context.cpp:53-65, 174-191`; `operation_registry.cpp:269-295`). Asio wake primitive는 Boost 소유다(`scheduler.ipp:674-685`).

| 옵션 | 바뀌는 규칙 하나 | 판정 위치 수 전→후 | 규칙 수 전→후 | 위험 |
|---|---|---:|---:|---|
| 기존 `io_context.run()`에서 ready handler 연속 실행을 사용 | 변경 없음. 이미 `run()` 반복이 한 번의 worker wake 사이에 여러 ready operation을 처리할 수 있다(`scheduler.ipp:218-224, 460-508`). | 1→1 (wake/drain은 Asio scheduler) | 1→1 | 추가 batch 효과는 큐에 ready item이 함께 있을 때만 있다. 현재 매 completion의 게시 사이에 idle이 발생하면 54% 비용은 그대로일 수 있다(가설, M5 §M5 줄 1-4; `scheduler.ipp:511-525`). S01 새 turn을 그대로 지킨다. 테스트: `test_cpp_framework_operation_registry.cpp:77-128` 독립 진행; 동일 wake 공유 테스트는 없음. |
| 기본 dispatcher pool을 더 작게 설정 | pool 크기 기본 규칙 하나를 낮춘다. | 1→1 (`provider_task_context.cpp:56-65`) | 1→1 | item마다 idle worker를 깨우는 동작은 없어지지 않는다. worker 포화·callback block 시 대기 증가 위험. 최소 2 worker로 막힌 callback과 독립 진행을 보장하려는 현재 근거가 있다(`provider_task_context.cpp:58-60`; test `test_cpp_framework_operation_registry.cpp:84-124`). |
| completion을 기존 coroutine/application executor에 게시 | 게시 executor 선택을 공유 dispatcher 대신 기존 executor로 바꾼다. | 2→1 (`provider_task_context.cpp:134-149, 174-191`; executor hook `:203-207`) | 1→1 | application worker가 막히면 completion도 막혀 S08 §4.4 독립 진행 및 worker 정책 유지(`08-messaging-hot-path.ko.md:324`)와 충돌할 수 있다. 또한 선택된 자원에서 turn/lane scope를 보존하는 증거가 없다. 구현·테스트 전에는 권고하지 않는다. |

**권고:** 첫 옵션처럼 Asio가 이미 제공하는 ready-item 반복 실행을 유지하고, 별도 serial drain이나 새 queue를 추가하지 않는다. 이는 현재 소스상 이미 적용된 규칙이며 wake 빈도 감소 효과를 입증하지 못한다. M5 수치만으로 pool 크기 감소나 다른 executor 전환의 성능 이득을 확정할 수 없다. PM이 성능 변경을 요구하면 후속 구현 승인 전에 복수 ready completion이 실제 한 wake에 함께 들어오는지 확인하는 focused 측정·테스트 조건을 먼저 정한다. 이 표에서는 새 flag·counter·queue·timer를 제안하지 않는다.
