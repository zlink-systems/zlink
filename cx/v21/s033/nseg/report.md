요청 제출→출발 Core 송신: Framework p50/p90 측정 불가, binding p50/p90 측정 불가.
상대 Core 수신→Long Running 수신 thread: Framework p50/p90 측정 불가, binding 전용 단계 없음.
Long Running thread→handler/reply 제출: Framework p50/p90 측정 불가, binding은 직접 수신 loop에서 처리.
상대 Core reply 송신→출발 Core 수신: Framework p50/p90 측정 불가, binding p50/p90 측정 불가.
출발 Core 수신→완료/호출자 continuation: Framework p50/p90 측정 불가, binding p50/p90 측정 불가.
가장 큰 Framework 추가 구간 후보는 출발 제출 경로와 target 수신 후 application dispatch이며, 각각 관리 코드 CPU는 보이나 요청별 대기 시간은 분리되지 않는다.

# .NET request-serial 1KiB 왕복 구간 진단

## 요약

이 분석은 `zlink-framework-dotnet-request-serial-1024`와 `zlink-dotnet-request-serial-1024`에서 보존한 스케줄러·시스템 호출 기록, CPU 표본, 결과 파일, 현재 소스 코드를 사용했다. 새 실행이나 빌드는 하지 않았다.

`window.json`의 3초 측정 구간에서 Framework는 23,760건이 완료되어 초당 7,920건, 요청 하나당 처리량 역수 126.3마이크로초였다. Binding은 37,827건이 완료되어 초당 12,609건, 처리량 역수 79.3마이크로초였다. 이 값은 구간 처리량에서 계산한 평균 상당 시간이며 중앙값이 아니다. 두 구간의 차이는 47.0마이크로초다.

별도 전체 벤치 결과는 Framework 초당 4,995건, 평균 지연 197.2마이크로초, 95백분위 지연 301마이크로초, 99백분위 지연 373마이크로초였다. Binding은 초당 8,647건, 평균 115.6마이크로초, 95백분위 181마이크로초, 99백분위 274마이크로초였다. `window.json`의 3초 처리량과 전체 결과의 처리량이 일치하지 않으므로 두 값을 같은 구간의 표본으로 합치지 않았다. 결과 파일은 중앙값과 90백분위 값을 제공하지 않는다.

## 구간별 수치

| 구간 | Framework p50 / p90 | Binding p50 / p90 | 관측된 경계와 해석 |
|---|---:|---:|---|
| 호출자 제출 시작→출발 Core 송신 | 측정 불가 | 측정 불가 | 제출은 .NET ThreadPool worker에서 시작하고 실제 소켓 송신은 `ZLINKbg/IO` thread의 `sendto`에서 확인된다. 제출 시작 표식은 없다. |
| 출발 송신→상대 Core 수신 | 측정 불가 | 측정 불가 | 출발 `sendto`와 상대 `recvfrom`은 확인되지만, 두 호출을 개별 요청에 대응시키는 식별자는 없다. |
| 상대 Core 수신→handler 진입 | 측정 불가 | 측정 불가 | Framework는 Core 수신 뒤 Long Running worker의 receive/dispatch 경로로 넘어간다. Binding은 수신 loop가 직접 처리한다. 관리 단계 진입 시각은 기록되지 않았다. |
| handler 실행→reply 제출 | 측정 불가 | 측정 불가 | CPU 표본에서 Framework route handler 호출과 binding `ReplyMultipart`를 확인했다. handler 시작·반환 표식은 없다. |
| 상대 reply 제출→상대 Core 송신→출발 Core 수신 | 측정 불가 | 측정 불가 | 송신·수신 시스템 호출은 확인되나 reply 제출 시각 및 요청별 호출 연결은 기록되지 않았다. |
| 출발 Core 수신→completion→호출자 continuation | 측정 불가 | 측정 불가 | Framework completion 처리와 ThreadPool work item 표본은 확인된다. Binding의 completion 경로도 CPU 표본에서 확인되지만, 완료·continuation 시각은 없다. |

스케줄러 파일에는 스케줄 전환, 깨우기, 시스템 호출 시각이 있고 CPU trace에는 약 1밀리초 간격의 표본이 있다. 두 자료 모두 관리 단계별 진입·종료 시각이나 요청 상관 식별자를 담지 않는다. 한 요청만 진행 중이어도 반복 poll, `EAGAIN`, 내부 wakeup 및 다른 관리 socket 호출이 섞여 있어서 가장 가까운 스케줄러 이벤트를 요청 단계 경계로 간주할 수 없다. 따라서 요청별 구간 중앙값·90백분위와 구간별 on-CPU 대 대기 시간을 수치로 산출할 수 없다.

## Thread와 시스템 호출 경로

시스템 호출 번호는 캡처 호스트의 x86-64 번호 체계다. 번호 44는 `sendto`, 번호 45는 `recvfrom`이다. 시스템 호출 인자와 파일 기술자는 16진수로 출력되고 반환값은 10진수다. 같은 thread에서 짧은 시간 간격으로 반복된 poll 호출과 `EAGAIN` 반환이 있어 모든 호출을 메시지 경계로 세지 않았다.

| 역할 | Framework thread | Binding thread | 확인 내용 |
|---|---|---|---|
| 출발 application submit | 프로세스 2912090의 `.NET TP Worker`(예: 2912131, 2912133, 2912172, 2912187, 2912199) | 프로세스 2914322의 `.NET TP Worker`(예: 2914359, 2914361, 2914383, 2914386) | CPU 표본에서 Framework의 `RequestNativeDurableWireAsync<TReply>` 및 binding의 `RawBenchTransport.RequestAsync`와 `SubmitRequest`가 보인다. |
| 출발 Core 송신 | `ZLINKbg/IO/0`, TID 2912139 | `ZLINKbg/IO/0` 또는 `ZLINKbg/IO/1`, TID 2914343·2914344 | Framework 출발 프로세스의 파일 기술자 `ad`에서 길이 1,425바이트 `sendto`가 관측됐다. |
| 상대 Core 수신 | `ZLINKbg/IO/0`, TID 2912010 | `ZLINKbg/IO/0` 또는 `ZLINKbg/IO/2`, TID 2914263·2914345 | Framework 상대 프로세스의 수신은 파일 기술자 `e0`의 `recvfrom` 경로다. 수신 뒤 Long Running worker를 깨우는 기록이 있다. |
| 상대 Long Running receive/dispatch 및 handler | 프로세스 2911960의 `.NET Long Running`, TID 2912030·2912037 | 전용 Long Running 단계 없음. `RunRequestRouter`는 `.NET TP Worker`, TID 2914267 | Framework CPU 표본은 `ReceiveUntilHandoff`, `ReceiveLoopAsync`, `DrainRawSocket`, route dispatch를 확인한다. Binding 표본은 직접 ROUTER 수신 loop를 확인한다. |
| 상대 Core reply 송신 | `ZLINKbg/IO/1`, TID 2912011 | `ZLINKbg/IO/0` 또는 `ZLINKbg/IO/1`, TID 2914263·2914264 | 상대 application의 reply 제출 이후 Core I/O thread가 송신을 맡는다. Framework 상대 프로세스의 송신 파일 기술자는 `f1`이다. |
| 출발 Core 수신 및 completion | 출발 `ZLINKbg/IO/2`, TID 2912141; completion poll/처리는 `.NET Long Running`, TID 2912147; completion work item은 `.NET TP Worker`, 예: 2912133 | 출발 프로세스 Core I/O thread 및 ThreadPool worker | Framework 표본에서 `CompletionOwner.Drain`, `RequestCompletionEntry.MoveReply`, `ZLinkCompletionDispatcher.WorkItem.Execute`를 확인했다. |
| 호출자 continuation | 출발 프로세스의 `.NET TP Worker` | 출발 프로세스의 `.NET TP Worker` | 두 구현 모두 벤치의 비동기 요청 loop로 돌아간다. 표본은 continuation을 포함하지만 개별 요청의 재개 시각을 주지 않는다. |

관측된 인과 경로는 출발 ThreadPool worker의 제출 → 출발 Core I/O worker의 송신 → 상대 Core I/O worker의 수신 → Framework Long Running worker 또는 Binding 직접 수신 loop → handler → 상대 Core I/O worker 송신 → 출발 Core I/O worker 수신 → completion 처리 → 호출자 continuation 순서다. 스케줄러 자료에서 깨우기 이벤트는 다수 보이지만 깨우기 주체가 CPU idle thread로 표시되는 경우가 많고, poller 내부 신호와 네트워크 도착을 요청 단위로 구분할 수 없다. 따라서 `A가 B를 깨웠다`는 관리 계층의 모든 연결은 단정하지 않고, 수신 뒤 Long Running worker가 깨는 순서만 Core 수신과 CPU 표본의 실행 경로를 함께 근거로 삼았다.

## 코드 경로와 CPU 근거

| 구간 | Framework 코드 | Binding 코드 | CPU 표본에서 확인한 비용 |
|---|---|---|---|
| 호출자 제출·요청 인코딩 | `framework/bench/grpc/dotnet/Client/Program.cs`: Framework transport의 `RequestAsync`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`: `RequestNativeDurableWireAsync<TReply>`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkDurableRequest.cs`: `RequestAsync` | `framework/bench/grpc/dotnet/Client/Program.cs`: `RawBenchTransport.RequestAsync`와 `SubmitRequest`; `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs`: `RequestAsync`; `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Request.cs`: `Async` | Framework source 표본에서 request helper, pending operation 등록/제출 경로와 envelope/protobuf 처리가 보인다. Binding 표본에서는 요청 제출, 응답 decode와 completion task continuation이 보인다. |
| Core 송신·상대 수신 | binding 경계의 `Systems.Zlink` Core 호출 뒤 Framework `ZLinkManagedMeshNode.ReceiveLoopAsync`와 `DrainRawSocket` | `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.ReceiveCore.cs`: receive 경로; `SocketKernel.Receive.cs`: public receive 경로 | CPU 표본만으로 native socket I/O의 벽시계 소요 시간을 분리하지 않았다. 스케줄러 기록의 `sendto`·`recvfrom`은 syscall 경계다. |
| Framework receive worker handoff | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkProcessExecutionPool.cs`: `RunWorker`, `ReceiveUntilHandoff`; `ZlinkManagedMeshNode.cs`: `ReceiveLoopAsync`, `ReceiveRawBatch`, `ProcessReceived`, `EnqueueOwned` | Binding에는 이에 대응하는 Framework receive pool/dispatch 단계가 없다. | Long Running thread 표본에서 `ReceiveUntilHandoff`, poller 대기, Framework raw receive 처리가 보인다. 실제 요청별 큐 대기와 깨우기 지연은 분리되지 않았다. |
| route dispatch와 handler | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs`: `DispatchNodeRouteAsync`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteHandlerInvoker.cs`: `InvokeRequestAsync`; `framework/bench/grpc/dotnet/ZLinkServer/Program.cs`: `EchoHandler.HandleAsync` | `framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs`: `RunRequestRouter`, `ReplyMultipart` | Framework Long Running thread 두 개의 표본에서 route batch dispatch, node route dispatch, request invoker가 반복 확인된다. Binding은 하나의 ThreadPool worker에서 receive loop와 reply 호출을 수행한다. |
| reply 제출·상대 Core 송신 | handler 반환 뒤 Framework route reply 경로에서 terminal reply를 준비하고 Core에 제출한다. CPU 표본에서 관련 route dispatch와 handler 경계를 확인했다. | `bindings/dotnet/src/Zlink/Runtime/Messaging/Received.Operations.cs`: `SendAsyncCore`; `bindings/dotnet/src/Zlink/Runtime/Messaging/RequestReplySupport.cs`: `SubmitPreservingOnFailure` | 상대 Core 송신은 해당 프로세스 `ZLINKbg/IO` thread의 syscall로 관측된다. 제출부터 syscall까지의 대기 시간은 없다. |
| 출발 Core 수신·completion | `Systems.Zlink.CompletionOwner.Drain`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs`: completion work item 실행 | `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs`: `Drain`, `RequestCompletionEntry`; `bindings/dotnet/src/Zlink/Runtime/Messaging/RequestReplySupport.cs` | Framework CPU 표본은 native reply 이동, `RequestCompletionEntry.MoveReply`, ThreadPool work item을 확인한다. Binding 표본은 `CompletionOwner.Drain`과 `RequestAsync` task 경로를 확인한다. |
| 호출자 continuation | `framework/bench/grpc/dotnet/Client/BenchMeasurement.cs`: `ExecuteRequestAsync`, `ObserveRequestReplyAsync` | 같은 벤치 파일과 `RawBenchTransport.RequestAsync` | 양쪽 모두 .NET async state machine과 ThreadPool 실행 비용이 표본에 포함된다. continuation 재개까지의 지연은 별도 timestamp가 없어 측정 불가다. |

## Framework 추가 비용이 큰 두 구간

첫 번째 후보는 출발 호출자 제출부터 출발 Core 송신까지다. Framework는 `RequestNativeDurableWireAsync<TReply>`에서 pending operation 생성·등록, deadline/admission 관리, durable request 경로, envelope 생성과 제출을 거친다. Binding은 `CompletionOwner.RequestAsync`에서 completion entry를 등록하고 `SubmitRequest`를 호출한다. Framework CPU 표본에서 요청 관리와 envelope/protobuf 함수가 확인되므로 on-CPU 비용은 존재한다. 기다림은 ThreadPool caller에서 Core I/O thread로 넘어가는 경로에 있지만, wake-to-run 시간을 요청별로 연결할 수 없어 수치가 없다.

두 번째 후보는 상대 Core 수신부터 handler 실행까지다. Framework는 Core receive 이후 `ReceiveLoopAsync`, raw batch 수신, owned record 처리, mailbox enqueue, process execution pool handoff, route dispatch와 handler invoker를 지난다. Binding은 `RunRequestRouter`에서 수신과 handler 호출을 직접 이어 간다. Framework의 Long Running thread CPU 표본은 이 추가 dispatch 경로를 뚜렷하게 보인다. Framework Long Running worker의 깨우기 이벤트도 기록되지만 poller·다른 관리 이벤트와 섞여 있어 요청별 대기 시간은 산출하지 않았다.

이 두 항목은 코드 단계와 CPU 표본을 기준으로 한 가장 큰 Framework 추가 비용 후보다. 구간별 p50/p90이나 on-CPU 대 대기 비율을 뜻하지 않는다. 현 캡처만으로 구간 간 차이를 마이크로초 단위로 순위화할 수 없다.

## 자료 한계와 완료 항목

`source.nettrace`와 `target.nettrace`는 CPU 표본이며 wall-clock span trace가 아니다. `sched.txt`는 syscall과 runnable 상태 변화를 기록하지만 Framework의 관리 코드 이벤트는 기록하지 않는다. 결과 파일은 평균·95백분위·99백분위 종단 간 지연을 제공하고 중앙값·90백분위 및 단계별 지연은 제공하지 않는다. 그러므로 요청별 p50/p90과 두 구간의 CPU/대기 수치가 이번 증거에서 빠진 실제 자료다.

출력 파일은 이 보고서 하나다. 변환한 임시 CPU 표본 파일은 제거했다. 코드 변경 없음.
