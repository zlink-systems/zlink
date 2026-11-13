# .NET request path counts and sources — #1639

## 범위와 측정 조건

- Worktree: `/root/worktree/zlink-1639d`, `HEAD=fb254a964c5bb5a1ccd7949e012f6830d8be5925` (`origin/main` detached).
- Benchmark 소스는 worktree 밖 `/root/jobs/fx/cx/v21/s033/dcount/overlay3/`에 복사했다. CPU 조건은 source 0–7, target 8–15다. worktree의 추적 파일은 바뀌지 않았다.
- Baseline은 `framework/bench/grpc/dotnet/run_local.sh`와 같은 runner를 썼다. 각 셀은 30초 warmup, 5초 active, 3회 측정했으며 각 invocation은 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 실행했다.
- Build는 `flock -s /root/jobs/fx/cx/measure.lock bash framework/bench/grpc/dotnet/build.sh`로 했다. MemAvailable 최저치는 15.25 GiB로 12 GiB 기준을 넘었다.
- 모든 36개 baseline 셀에서 client/server 오류가 0이고 완료 수와 수신 수가 같았다. 자료는 `raw/baseline-results.json`과 각 `raw/base-r*-*.log`, 결과 JSON에 있다.

## Fresh baseline throughput

단위는 message/s다. 비율은 같은 셀의 Framework 중앙값 ÷ binding 중앙값이다.

| 패턴 / payload | Framework r1 / r2 / r3 | Framework 중앙값 | Binding r1 / r2 / r3 | Binding 중앙값 | Framework ÷ binding |
|---|---:|---:|---:|---:|---:|
| request-backpressure / 1024 B | 29,921 / 28,051 / 29,511 | 29,511 | 62,202 / 58,323 / 58,304 | 58,323 | 0.506 |
| request-backpressure / 4096 B | 24,250 / 24,225 / 24,212 | 24,225 | 48,867 / 40,953 / 49,159 | 48,867 | 0.496 |
| request-serial / 1024 B | 5,831 / 5,878 / 6,078 | 5,878 | 11,065 / 10,877 / 12,244 | 11,065 | 0.531 |
| request-serial / 4096 B | 5,795 / 4,977 / 4,272 | 4,977 | 10,480 / 10,252 / 11,238 | 10,480 | 0.475 |
| send-saturation / 1024 B | 183,719 / 183,271 / 183,540 | 183,540 | 229,539 / 230,208 / 229,711 | 229,711 | 0.799 |
| send-saturation / 4096 B | 137,132 / 132,839 / 134,139 | 134,139 | 152,757 / 157,453 / 168,053 | 157,453 | 0.852 |

request-backpressure와 request-serial 네 셀의 비율은 각각 0.6 목표보다 낮다. 두 send-saturation 셀은 0.6을 넘는다.

## request-backpressure / 1024 B per-request counts

아래 baseline 평균값은 3회 active 결과에서 계산했다. CPU는 benchmark의 `Process.TotalProcessorTime` 차이를 완료 수로 나눈 값이다. allocation bytes는 runtime `GC.GetTotalAllocatedBytes(precise: true)` 차이, work item은 `ThreadPool.CompletedWorkItemCount` 차이를 완료 수로 나눴다. source/target의 binding 행은 `zlink-dotnet`이다.

| 구현 / 역할 | CPU µs / 완료 요청 | Allocated bytes / 요청 | ThreadPool completed work items / 요청 | Monitor contention events / 요청¹ |
|---|---:|---:|---:|---:|
| Framework source | 200.3 | 10,204 | 2.076 | 약 0.00041 |
| Framework target | 95.7 | 4,962 | 0.0035 | 약 0.00455 |
| Binding source | 82.2 | 3,106 | 1.001 | 약 0.00030 |
| Binding target | 36.8 | 1,456 | 0.000014 | 0 |

¹ Current-main profile의 `System.Runtime` Monitor Lock Contention Count rate를 profiled throughput으로 나눴다. 이는 경합이 발생한 Monitor acquisition 수이며, 경합 없이 끝난 모든 lock/acquisition 수는 아니다. Counter는 1초 rate이므로 per-request 값은 근삿값이다.

세 라운드의 allocation bytes/request는 Framework source 10,204 / 10,250 / 10,195, target 4,962 / 4,946 / 4,972였다. Binding source는 3,106 / 3,106 / 3,107, target은 1,456 / 1,456 / 1,456이었다. 별도의 runtime counter allocation-rate도 current-main diagnostic capture에서 이 값과 같은 크기를 보였다.

정확한 managed object allocation **개수**는 수집되지 않았다. `GC.GetTotalAllocatedBytes`와 `System.Runtime` allocation-rate는 byte 수를 제공하지만 객체 수를 제공하지 않는다. `GCAllocationTick` 표본을 객체 개수로 환산하지 않았다.

ThreadPool 수는 완료된 work item의 개수다. continuation별 schedule 횟수와 같은 값은 아니다. Backpressure 1024에서 Framework source는 완료 요청 하나당 약 2.076 work item, binding source는 약 1.001개를 처리했다. target은 각각 0.0035, 거의 0개였다.

## Per-thread CPU와 profile counters

현재 `fb254a964c`에서 1024 B backpressure를 30초 warmup / 5초 active로 한 차례 더 실행하며 EventPipe CPU sampling, `System.Runtime` counters, thread CPU ticks를 수집했다. 이 진단 실행은 profiler overhead가 있으므로 위 baseline 중앙값에 넣지 않았다. 프로파일된 실행의 CPU는 source/target 순으로 Framework 217.9/106.2 µs/request, binding 95.1/42.4 µs/request였다.

각 프로세스에서 CPU를 가장 많이 쓴 thread의 CPU µs/completed request는 다음과 같다. 괄호 안은 active 구간의 한 core 대비 점유율이다.

| 구현 / 역할 | 상위 thread CPU µs / 요청 |
|---|---|
| Framework source | `ZLINKbg/IO/0` 23.0 (0.52), `ZLINKbg/IO/2` 18.2 (0.42), `.NET Long Runni` 16.9 (0.39), `.NET TP Worker` 9.2 (0.21), `.NET TP Worker` 9.1 (0.21), `ZLINKbg/IO/1` 8.3 (0.19) |
| Framework target | `ZLINKbg/IO/1` 21.7 (0.50), `ZLINKbg/IO/0` 17.5 (0.40), `.NET Long Runni` 16.7 (0.38), `.NET Long Runni` 16.5 (0.38), `.NET Long Runni` 16.5 (0.38) |
| Binding source | `.NET TP Worker` 19.0 (0.86), `ZLINKbg/IO/0` 16.1 (0.73), `ZLINKbg/IO/2` 10.0 (0.45), `.NET TP Worker` 6.8 (0.31), `.NET TP Worker` 6.1 (0.28) |
| Binding target | `ZLINKbg/IO/0` 13.7 (0.62), `ZLINKbg/IO/1` 11.9 (0.54), `.NET TP Worker` 11.1 (0.50) |

이 측정에서는 Framework thread 하나가 한 core를 포화하지 않았다. `ZLINKbg/IO/*`는 binding/Core I/O worker다. managed sample stack은 해당 native worker 안의 Core 작업을 분리하지 못했다.

Current-main runtime counter에서 Monitor contention은 Framework source 약 9.5 events/s, target 약 105.5 events/s였다. Binding은 source 약 14 events/s, target은 0에 가까웠다. Completed ThreadPool work item rate는 Framework source 약 49,000/s, target 약 72/s였고, binding은 source 약 47,000/s, target은 0에 가까웠다. `raw/profile-raw/` 아래 nettrace와 counter JSON에 raw 결과가 있다.

Syscall 수는 current-main profile의 `perf stat` 출력 파일이 비어 있어 얻지 못했다. 이용 가능한 가장 가까운 .NET profile(`/root/jobs/fx/cx/v21/s033/dprof/`, main `ee11390257`)에서 request-backpressure 1024의 syscall entry/request는 Framework source/target 42.6/11.8, binding source/target 28.3/5.5였다. 이 syscall 값은 현재 `fb254a964c`의 fresh 값이 아니다. 해당 capture의 다른 측정값도 commit 차이가 있으므로 current-main 수치로 해석하지 않는다.

## Framework-only top call sites

현재-main EventPipe CPU sample의 self stack 비중을 그 profiled 프로세스의 `Process.TotalProcessorTime`에 비례해 환산했다. 따라서 아래 CPU 기여량은 sample 기반 추정치다. 모든 site는 framework source 또는 target 프로세스에서 관측한 `Zlink.Framework` site이며 Framework-only로 분류했다.

Source 프로세스의 Framework/binding profiled CPU 차이는 약 122.8 µs/request다. 아래 19개 site의 sample 기반 합은 약 99.2 µs/request(차이의 80.8%)다.

| Site — function, file:line | CPU 추정 µs/request | 담당하는 Framework 결정 |
|---|---:|---|
| `ZLinkManagedMeshNode.RequestToNodeDirectAsync<TReply>` — `ZLinkManagedMeshNode.cs:10696` | 39.62 | 직접 node request를 native durable request 경로로 넘긴다. |
| `ZLinkManagedMeshNode.RequestNativeDurableWireAsync<TReply>.MoveNext` — `ZLinkManagedMeshNode.cs:11546` | 7.45 | native request lifetime을 얻고 reply/terminal 경로를 기다린다. |
| `ZLinkManagedMeshNode.RunDeadlineRequestAsync<TReply>` — `ZLinkManagedMeshNode.cs:11204` | 7.26 | pending operation과 deadline을 등록하고 terminal 결과를 수렴한다. |
| `ZLinkManagedMeshNode.DecodeDirectApplicationReply` — `ZLinkManagedMeshNode.cs:11409` | 6.55 | 직접 reply의 application payload를 decode한다. |
| `RequestNativeDurableWireAsync.ProduceAsync` — `ZLinkManagedMeshNode.cs:11546` | 5.17 | 선택한 peer에 native request를 제출하고 reply를 만든다. |
| `ZLinkRouteRequestCall<TRequest>.ExecuteAsync<TReply>` — `ZLinkRouteClient.cs:403` | 5.12 | typed request를 envelope로 encode하고 대상 node에 전달한다. |
| `ZLinkManagedMeshNode.CreateOperationAsync` — `ZLinkManagedMeshNode.cs:5586` | 4.22 | operation ID와 pending operation을 owner state에 등록한다. |
| `ZLinkStateLane.RunAsync<T>` — `ZLinkStateLane.cs:67` | 3.90 | lane 소유 상태 변경을 한 turn에서 수행한다. |
| `ZLinkEnvelopeCodec.ReadProtocolHeader` — `ZLinkEnvelopeCodec.cs:373` | 3.04 | inbound envelope의 protocol header를 읽고 확인한다. |
| `ZLinkDurableRequest.RequestAsync<TReply>` — `ZLinkDurableRequest.cs:8` | 2.56 | binding request/reply API를 호출하고 결과를 기다린다. |
| `ZLinkSpotNodeRuntime.RequestToNodeAsync` — `ZLinkSpotNodeRuntime.cs:801` | 2.35 | target node request를 managed mesh node로 전달한다. |
| `ZLinkManagedMeshNode.SubmitNativeWireAsync` — `ZLinkManagedMeshNode.cs:11294` | 2.26 | wire frames를 현재 peer의 native route에 제출한다. |
| `ZLinkRawRouterServicePort.CreateMessages` — `ZLinkRawRouterServicePort.cs:195` | 2.22 | serialized wire parts에서 binding `Message` 배열을 만든다. |
| `ZLinkStateLane.RunAsync(Action)` — `ZLinkStateLane.cs:140` | 1.53 | action 형태의 lane-owned turn을 수행한다. |
| `RequestNativeDurableWireAsync` completion callback — `ZLinkManagedMeshNode.cs:11546` | 1.49 | 종료된 native request의 completion을 owner state에 반영한다. |
| `ZLinkRouteClient.RequestToNode<TRequest>` — `ZLinkRouteClient.cs:75` | 1.33 | public route request call을 만든다. |
| `CreateOperationAsync` cancellation/deadline callback — `ZLinkManagedMeshNode.cs:5586` | 1.12 | caller cancellation이나 operation terminal을 등록된 operation에 반영한다. |
| `ZLinkStateLane.TryPost<TState>` — `ZLinkStateLane.cs:197` | 1.03 | owner turn 작업을 lane mailbox에 넣는다. |
| `PendingOperation<TReply>..ctor` — `ZLinkManagedMeshNode.cs:14032` | 1.01 | pending operation의 ID, kind, source fence와 completion을 초기화한다. |

Target 프로세스의 Framework/binding profiled CPU 차이는 약 63.9 µs/request다. 아래 23개 site의 합은 약 46.0 µs/request(차이의 72.0%)다. 이 프로파일에서 관측된 모든 `Zlink.Framework` self site 합도 약 47.1 µs/request(73.8%)로, 80%인 약 51.1 µs/request에 미치지 못했다. 남은 차이는 managed Framework method에 귀속되지 않은 runtime, binding/Core native, kernel sample에 분산돼 있다.

| Site — function, file:line | CPU 추정 µs/request | 담당하는 Framework 결정 |
|---|---:|---|
| `ZLinkManagedMeshNode.EnqueueOwned` — `ZLinkManagedMeshNode.cs:12783` | 7.99 | inbound record를 mailbox에 넣고 readiness를 갱신한다. |
| `ZLinkMeshDispatchPump.EnqueueNodeRoute` — `ZLinkMeshDispatchPump.cs:707` | 6.30 | node route record의 metadata와 reply/payload ownership을 application route로 넘긴다. |
| `ZLinkManagedMeshNode.ReceiveRawBatch` — `ZLinkManagedMeshNode.cs:6319` | 5.04 | binding에서 받은 batch를 managed ingress record로 처리한다. |
| `ZLinkManagedMeshNode.ProcessReceived` — `ZLinkManagedMeshNode.cs:7048` | 4.15 | record kind와 request metadata에 따라 inbound 경로를 선택한다. |
| `ZLinkRouteHandlerInvoker.InvokeRequestAsync` — `ZLinkRouteHandlerInvoker.cs:93` | 4.01 | admitted request를 route handler에 전달한다. |
| `ZLinkHandlerDispatcher.InvokeUnfilteredAsync` — `ZLinkHandlerDispatcher.cs:86` | 2.63 | 선택된 handler를 호출하고 결과를 수집한다. |
| `ZLinkEnvelopeCodec.ReadProtocolHeader` — `ZLinkEnvelopeCodec.cs:373` | 2.59 | inbound application envelope header를 읽고 확인한다. |
| `ZLinkMeshNodeRouteDispatcher.DispatchNodeRouteAsync` — `ZLinkMeshNodeRouteDispatcher.cs:602` | 2.23 | node request를 target handler/route에 dispatch한다. |
| `ZLinkMeshNodeRouteDispatcher.DispatchAsync` — `ZLinkMeshNodeRouteDispatcher.cs:473` | 1.76 | route record 종류와 대상에 맞춰 dispatch 경로를 선택한다. |
| `ZLinkRouteHandlerInvoker.InvokeRequestAsync` — `ZLinkRouteHandlerInvoker.cs:123` | 1.69 | handler 입력과 dispatch context를 구성한다. |
| `ZLinkManagedMeshNode.DrainReadyAsync` — `ZLinkManagedMeshNode.cs:2771` | 1.17 | 현재 ready domain에서 처리할 batch를 선택한다. |
| `ZLinkManagedMeshNode.PrepareNativeTerminalReply` — `ZLinkManagedMeshNode.cs:12213` | 0.85 | handler 결과를 native terminal reply로 만든다. |
| `ZLinkServiceWireCodec.EncodeReply` — `ZLinkServiceWireCodec.cs:390` | 0.80 | service reply frame을 encode한다. |
| `ZLinkApplicationJobQueueInvocation.Enter` — `ZLinkApplicationJobQueue.cs:1330` | 0.76 | handler invocation 동안 application queue lease scope를 설정한다. |
| `ZLinkEnvelopeCodec.EncodePlannedHeader` — `ZLinkEnvelopeCodec.cs:1154` | 0.68 | reply envelope header bytes를 만든다. |
| `ZLinkEnvelopeCodec.EncodeParts` — `ZLinkEnvelopeCodec.cs:148` | 0.57 | typed reply를 multipart envelope로 encode한다. |
| `ZLinkManagedMeshNode.SubmitCapturedNativeReply` — `ZLinkManagedMeshNode.cs:12297` | 0.53 | 준비된 native reply submission을 수행한다. |
| `ZLinkHandlerInvocationEngine.InvokeAsync` — `ZLinkHandlerInvocationEngine.cs:7` | 0.47 | handler method와 argument plan을 실행한다. |
| `ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage` — `ZLinkApplicationPayloadEnvelopeCodec.cs:140` | 0.45 | multipart application payload를 framework envelope로 감싼다. |
| `ZLinkManagedMeshNode.CollectInfrastructureUnderLock` — `ZLinkManagedMeshNode.cs:10319` | 0.42 | 선택된 route hello와 peer liveness activity를 수집한다. |
| `ZLinkProcessExecutionPool.ReceiveSource.WaitAsync` — `ZLinkProcessExecutionPool.cs:843` | 0.34 | worker의 receive/readiness event를 기다린다. |
| `ZLinkMeshNodeRouteDispatcher.DispatchBatchAsync` — `ZLinkMeshNodeRouteDispatcher.cs:226` | 0.28 | 받은 node route batch를 순서에 따라 dispatch한다. |
| `ZLinkMeshNodeOwnedMailbox.ClaimAndDrainOnLane` — `ZLinkMeshOwnedMailbox.cs:104` | 0.27 | lane owner가 다음 처리 가능한 mailbox record들을 claim한다. |

### Shared binding sites

아래 심볼은 Framework-only site가 아니라 Framework와 binding 양쪽에서 관측된 `Systems.Zlink` package site다. 이 worktree에는 해당 package의 implementation source와 file:line이 없어 심볼 이름만 기록했다.

| Shared binding function | 관측된 역할 |
|---|---|
| `Systems.Zlink.CompletionOwner.RequestAsync` | native request를 submit하고 completion owner에 reply를 기다린다. |
| `Systems.Zlink.Message.MoveFromNative` / `CompletionOwner.RequestCompletionEntry.MoveReply` | native completion payload를 managed message로 옮긴다. |
| `Systems.Zlink.Poller.Wait` | socket/poller readiness를 기다린다. |
| `Systems.Zlink.Runtime.Sockets.Internal.SocketKernel.ReceiveParts` | routed multipart frames를 native socket에서 받는다. |
| `Systems.Zlink.RequestReplySupport.SubmitPreservingOnFailure` | reply를 submit하고 실패 시 frame ownership을 보존한다. |

## 제한과 산출물

- Current-main의 정확한 per-object allocation count와 syscall/request는 측정하지 못했다. allocation byte, ThreadPool completed work item, Monitor contention, CPU samples는 현재 main에서 측정했다. syscall 수만 별도 표기한 이전 profile의 값이다.
- Current-main target 쪽 Framework method sample만으로 Framework/binding CPU 차이의 80%를 덮지 못했다. 요청한 80% 기준에 도달한 것처럼 추정치를 부풀리지 않았다.
- Build에서 기존 nullable/XML comment warnings가 있었지만 build는 성공했다. Test suite는 요청되지 않아 실행하지 않았다.
- `git status --short`는 비어 있다. 소스 변경, commit, branch 변경은 없다.
- Logs, traces, counters, raw results 및 runner 복사본은 `/root/jobs/fx/cx/v21/s033/dcount/` 아래에 있다.

리팩토링 점검: 성능 4건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 없음, 넘긴 항목: 목표 미달 request 셀 4건은 측정 결과로 기록했다.
