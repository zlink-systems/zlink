# .NET request-serial 측정 보고서

## 1. 실제 Framework 경로와 실행 스레드

| 순서 | 실제 경로와 위치 | 실행 스레드 또는 executor |
|---|---|---|
| 호출자 제출 | bench의 `ExecuteRequestAsync`가 `transport.RequestAsync`를 기다린다. `BenchMeasurement.cs:557-582` | bench 측 .NET ThreadPool worker. continuation은 await 이후 ThreadPool worker에서 실행될 수 있다. |
| Framework client 호출 | `FrameworkBenchTransport.RequestAsync` → `SubmitRequest` → `client.RequestToNode(...).Async<BenchPayload>()`. `Client/Program.cs:342-351` | 호출한 bench worker에서 동기 부분을 시작한다. |
| typed envelope와 payload encode | `ZLinkRouteRequestCall<T>.ExecuteAsync`가 envelope를 만들고 `ZLinkClientCallCodec.EncodeEnvelopeParts`를 호출한다. `ZLinkRouteClient.cs:403-473`, `ZLinkClientCallCodec.cs:46-58` | source의 .NET managed worker. encode는 이 동기 구간에서 끝난다. |
| service wire encode | NodeRequest가 `ZLinkSpotNodeRuntime`와 backend wrapper를 지나 `RequestToNodeDirectAsync`로 들어간다. `ZLinkSpotNodeRuntime.cs:821-845`, `Wrappers/ZLinkBackendSpotNodeWrapper.cs:1335-1356`, `ZLinkManagedMeshNode.cs:10714-10753`. `CreateApplicationWire`가 service multipart frame을 만든다. `ZLinkManagedMeshNode.cs:10917-10943` | source managed worker. |
| binding submit | `RequestNativeDurableWireAsync`가 `socket.Request(target).Messages(messages).Timeout(timeout)`을 제출한다. `ZLinkManagedMeshNode.cs:11330-11358` | submit 동기 부분은 source managed worker. 실제 socket send는 별도의 `ZLINKbg/IO/*` native I/O thread에서 보였다. |
| target receive와 mailbox enqueue | `ReceiveLoopAsync`의 `_socket.Recv` 뒤 `ProcessReceived`가 service frame을 읽고 `EnqueueOwned`를 호출한다. `ZLinkManagedMeshNode.cs:6191-6200, 6345-6359, 7061, 7346-7428` | target의 `ZLINKbg/IO/*` thread가 native receive를 수행한다. managed receive loop는 `.NET Long Running Task`로 보였다. parse와 enqueue는 receive loop에서 진행된다. |
| mailbox claim과 Node route dispatch | `ZLinkMeshDispatchPump`가 `claim.ReceiveAsync`로 application mailbox claim을 가져오고 `DispatchNodeRoutes`를 실행한다. `ZLinkMeshDispatchPump.cs:495-509, 545-579, 754` | mailbox owner의 bounded claim turn과 application dispatch worker. 측정에서 enqueue 다음 dispatch marker의 managed ThreadId가 바뀌었다. |
| handler와 Framework reply encode | `DispatchNodeRouteAsync`가 `_routeInvoker.InvokeRequestAsync`를 호출하고 `ReplyResponseAsync`로 응답한다. `ZLinkMeshNodeRouteDispatcher.cs:538-543, 623-675, 776`. 실제 handler는 `ZLinkServer/Program.cs:62-73`의 `EchoHandler`다. native reply는 `CreateNativeReplyHandler`와 `PrepareNativeTerminalReply` 경로를 사용한다. `ZLinkManagedMeshNode.cs:7433-7460, 12365-12385` | application dispatch worker에서 handler와 reply 준비가 실행된다. reply의 실제 native send는 target `ZLINKbg/IO/*` thread에서 보였다. |
| source receive와 Framework completion | 물리 reply를 기다린 뒤 decode하고 `CompleteManagedOperation`을 호출한다. `ZLinkManagedMeshNode.cs:11145-11155, 11158-11200`. Completion은 `ZLinkCompletionDispatcher.Post`를 거쳐 ThreadPool work item에서 실행된다. `ZLinkCompletionDispatcher.cs:13-17, 25-59` | source native I/O thread가 reply frame을 받는다. physical reply decode는 기다린 source managed worker에서 이어지고 completion은 ThreadPool worker에 게시된다. |
| caller continuation | `ZLinkRouteClient`의 reply decoder가 typed reply를 만들고 요청의 `ValueTask`를 완료한다. `ZLinkRouteClient.cs:456-473`; bench await는 `BenchMeasurement.cs:574-583` | completion worker에서 continuation이 inline 실행될 수 있고 다음 bench await는 ThreadPool worker에서 이어진다. |

이 요청은 public `RequestToNode`를 이용한 RouteMesh `NodeRequest`다. `DispatchChannelAsync`의 RouteMesh channel handler 경로는 사용하지 않는다. `ZLinkMeshDispatchPump`의 일반 application mailbox claim과 Node route dispatcher는 실제로 사용한다. 그러므로 #3995의 RouteMesh channel handler / mailbox claim turn 변경 중 channel handler 최적화는 이 serial 경로에 없다. mailbox claim 자체는 경로에 있다.

## 2. Framework와 binding의 구간별 시간

각 셀은 `p50 / p90 / mean` µs다. 추적은 condition B, source CPU 0–7과 target CPU 8–15에서 수행했다. 네 셀은 같은 session에서 각각 RUNS=1, 15초 warmup, 5초 측정으로 실행했고, 각 capture 중 3초 구간의 syscall trace와 managed marker를 맞췄다. 1KiB는 1,024 payload bytes, 4KiB는 4,096 payload bytes다. 표의 구간은 같은 요청 안의 연속 timestamp 차이다.

| 구현과 구간 | 1KiB p50/p90/mean | 4KiB p50/p90/mean |
|---|---:|---:|
| Framework bench→Framework call | 0.642 / 0.982 / 0.889 | 0.711 / 0.982 / 0.843 |
| binding bench→raw request setup | 0.100 / 0.160 / 0.152 | 0.091 / 0.160 / 0.155 |
| Framework call→typed encode done | 3.356 / 5.079 / 4.115 | 3.567 / 5.330 / 4.128 |
| binding raw setup→body encode done | 0.952 / 1.333 / 1.104 | 1.162 / 1.763 / 1.327 |
| Framework typed encode→service wire encode done | 0.441 / 0.571 / 0.662 | 0.431 / 0.531 / 0.602 |
| Framework service wire encode→native submit | 0.832 / 1.192 / 0.894 | 1.052 / 1.463 / 1.294 |
| binding body encode→binding submit | 0.050 / 0.080 / 0.106 | 0.060 / 0.080 / 0.110 |
| Framework native submit→source sendto | 17.271 / 24.355 / 18.755 | 18.034 / 24.465 / 19.536 |
| binding binding submit→source sendto | 11.631 / 16.130 / 12.613 | 13.144 / 18.204 / 13.929 |
| source sendto→target recvfrom, Framework | 21.209 / 27.370 / 22.059 | 22.381 / 27.821 / 23.048 |
| source sendto→target recvfrom, binding | 24.375 / 30.046 / 25.319 | 26.609 / 33.381 / 27.674 |
| Framework target recvfrom→ProcessReceived | 23.403 / 30.346 / 23.893 | 23.082 / 31.098 / 24.015 |
| binding target recvfrom→handler start | 11.080 / 15.749 / 12.150 | 12.213 / 17.853 / 13.319 |
| Framework receive parse→mailbox enqueue | 1.072 / 1.583 / 1.206 | 1.082 / 1.653 / 1.246 |
| Framework mailbox enqueue→Node route dispatch | 18.804 / 25.938 / 20.314 | 18.068 / 24.656 / 19.909 |
| Framework Node dispatch/handler/reply encode | 6.362 / 9.287 / 6.785 | 7.484 / 10.570 / 7.931 |
| binding handler decode/echo→reply call | 0.340 / 1.693 / 0.982 | 1.222 / 2.435 / 1.423 |
| Framework reply submit→target sendto | 13.244 / 18.504 / 14.045 | 13.074 / 19.449 / 14.076 |
| binding reply call→target sendto | 11.712 / 16.139 / 12.596 | 12.714 / 18.514 / 13.966 |
| target sendto→source recvfrom, Framework | 27.541 / 34.163 / 28.423 | 28.342 / 35.255 / 29.327 |
| target sendto→source recvfrom, binding | 18.153 / 22.061 / 18.775 | 25.126 / 32.691 / 26.400 |
| Framework source recvfrom→physical reply observed | 37.859 / 47.167 / 38.958 | 38.100 / 47.467 / 39.410 |
| Framework reply decode→completion queue | 5.450 / 7.664 / 5.627 | 6.091 / 8.426 / 6.432 |
| binding source recvfrom→binding completion/typed decode | 27.280 / 33.161 / 28.152 | 30.245 / 38.058 / 31.826 |
| Framework completion queue→worker execute | 0.341 / 0.881 / 0.753 | 0.351 / 2.635 / 0.858 |
| Framework completion execute→caller/next request | 3.286 / 4.899 / 3.673 | 5.670 / 7.865 / 6.796 |
| binding completion→caller/next request | 1.573 / 2.745 / 2.193 | 3.968 / 5.750 / 4.639 |

각 요청의 구간 mean을 더한 값은 동일한 D4 marker 사이에서 잰 serial 요청 mean과 일치한다. Framework 1KiB는 구간 합 191.051µs, 요청 간격 191.048µs로 오차 0.003µs다. Framework 4KiB는 둘 다 199.451µs로 표시 정밀도 이내다. Binding 1KiB는 구간 합과 요청 간격이 114.142µs이고, 4KiB는 134.768µs다. 구간별 유효 cycle은 각각 15,749, 15,078, 26,330, 22,313개다.

추적은 무추적 수치와 같지 않다. trace를 켠 요청 간격 mean은 Framework 191.048/199.451µs, binding 114.142/134.768µs다. 이는 아래의 무추적 RUNS=3 중앙값보다 각각 46.016/47.642µs, 26.190/42.807µs 높다. 따라서 이 trace는 구간 위치를 보는 용도이며 절대 성능 기준은 다음 무추적 결과로 둔다.

## 3. 1KiB에서 4KiB로 늘 때의 증가, 복사와 할당

trace 구간 mean 증가량은 Framework의 Node dispatch/handler/reply encode가 +1.146µs, reply decode→completion queue가 +0.805µs, completion execute→caller가 +3.123µs다. binding은 handler decode/echo→reply call이 +0.441µs, source receive→completion/typed decode가 +3.674µs, completion→caller가 +2.446µs다. 대응 handler 구간에서 Framework 증가가 binding보다 0.705µs 크고, caller continuation 구간은 0.677µs 크다. 다만 이 구간의 managed allocation 증가는 Framework dispatch 3,072 bytes와 binding handler 3,072 bytes, Framework caller continuation 3,072 bytes와 binding caller continuation 3,072 bytes로 같다. Framework는 multipart wrapping으로 본문 복사를 두 번 더 하며, 1→4KiB에 따른 추가 복사량 차이는 총 6,144 bytes/request다. 커널 syscall 사이의 왕복 구간은 Framework에서 요청 방향 +0.989µs, 응답 방향 +0.904µs 늘고 binding에서는 각각 +2.355µs, +7.625µs 늘었다. 이번 측정에서 Framework 고유의 큰 1→4KiB 시간 증가는 보이지 않는다. 증가한 managed allocation은 양쪽의 protobuf decode와 호출자 응답 materialization 구간에서 payload 증가분 3,072 bytes/request와 같은 크기로 나타난다.

바이트 복사는 payload 본문 크기 기준이다. Framework typed codec은 payload part를 만들고, `CreateApplicationWire`가 service multipart 메시지를 별도로 만들어 part 본문을 다시 쓴다. `EncodeFrameworkMultipartMessage`와 `WriteMultipartPayload`는 `ZLinkApplicationPayloadEnvelopeCodec.cs:153-163, 388-410`에 있다. 수신 측 multipart view는 `TryDecodeFrameworkMultipartView`에서 본문을 복제하지 않는다. `ZLinkApplicationPayloadEnvelopeCodec.cs:211-222`. 이후 protobuf parser가 객체/ByteString payload를 materialize한다. 따라서 각 방향에서 Framework는 직렬화 출력 1회, service multipart 포장 복사 1회, 수신 decode의 본문 복사 1회다. request와 reply 왕복으로 합계 여섯 번의 payload 크기 본문 이동이다.

Binding bench는 `Message.Allocate`한 body에 `RawWire.Encode`를 바로 쓴다. `Client/Program.cs:504-515`, `Shared/RawWire.cs:5-9`. target도 decode한 protobuf를 reply body에 바로 쓰고 두 message를 제출한다. `ZLinkRawServer/Program.cs:103-109`. 왕복 합계는 직렬화 출력 두 번과 decode materialization 두 번, 총 네 번의 payload 크기 본문 이동이다. Framework에만 service multipart wrapping이 두 방향에서 한 번씩 더 있으므로 추가 복사량은 1KiB에서 2,048 bytes/request, 4KiB에서 8,192 bytes/request다. payload 증가에 따른 복사량 차이는 6,144 bytes/request다. syscall trace로 사용자 메모리 copy 명령 자체를 계측한 것은 아니며, 이 수는 encode/decode 및 frame writer 경로에 있는 본문 크기 memory pass를 세어 산정했다.

동일 thread marker 사이의 `GC.GetAllocatedBytesForCurrentThread` 차이로 구간 할당 바이트를 계산했다. Framework의 Node dispatch/handler/reply encode 구간은 1,984→5,056 bytes/request, source reply decode→completion queue는 1,680→4,752 bytes/request, completion execute→caller는 1,112→4,184 bytes/request다. Binding handler decode/echo는 1,144→4,216 bytes/request, completion→caller는 1,112→4,184 bytes/request다. 앞의 네 payload 의존 차이는 모두 3,072 bytes/request다. encode marker의 managed allocation은 payload 크기에 따라 늘지 않았고, `Message`의 native storage는 GC allocated byte에 포함되지 않는다.

## 4. 구간별 thread handoff와 managed allocation

handoff 수는 각 구간의 시작과 끝에서 실행 thread가 달라진 요청 수다. 이는 OS scheduler context switch 횟수가 아니다. syscall marker는 Linux TID이고 managed marker는 .NET ManagedThreadId이므로 syscall을 가로지르는 행은 서로 다른 executor로 넘어간 것으로 계산했다. allocation 값은 같은 managed thread 양끝에 marker가 있는 경우만 표시했다. `N/A`는 할당이 0이라는 뜻이 아니라, 다른 executor로 넘어가 현재 thread별 allocation counter의 구간 차를 잴 수 없다는 뜻이다. 값은 1KiB→4KiB bytes/request이며, handoff는 각 셀의 전체 유효 cycle 중 횟수다.

| 구현과 구간 | handoff 1KiB→4KiB | allocation bytes/request 1KiB→4KiB |
|---|---:|---:|
| Framework bench→Framework call | 0→0 | 152→152 |
| binding bench→raw request setup | 0→0 | 0→0 |
| Framework call→typed encode done | 0→0 | 320→320 |
| binding raw setup→body encode done | 0→0 | 64→64 |
| Framework typed encode→service wire encode done | 0→0 | 304→304 |
| Framework service wire encode→native submit | 0→0 | 104→104 |
| binding body encode→binding submit | 0→0 | 0→0 |
| Framework native submit→source sendto | 15,749→15,078 | N/A→N/A |
| binding binding submit→source sendto | 26,330→22,313 | N/A→N/A |
| source sendto→target recvfrom, Framework | 15,749→15,078 | N/A→N/A |
| source sendto→target recvfrom, binding | 26,330→22,313 | N/A→N/A |
| Framework target recvfrom→ProcessReceived | 15,749→15,078 | N/A→N/A |
| binding target recvfrom→handler start | 26,330→22,313 | N/A→N/A |
| Framework receive parse→mailbox enqueue | 0→0 | 232→232 |
| Framework mailbox enqueue→Node route dispatch | 15,749→15,078 | N/A→N/A |
| Framework Node dispatch/handler/reply encode | 0→0 | 1,984→5,056 |
| binding handler decode/echo→reply call | 0→0 | 1,144→4,216 |
| Framework reply submit→target sendto | 15,749→15,078 | N/A→N/A |
| binding reply call→target sendto | 26,330→22,313 | N/A→N/A |
| target sendto→source recvfrom, Framework | 15,749→15,078 | N/A→N/A |
| target sendto→source recvfrom, binding | 26,330→22,313 | N/A→N/A |
| Framework source recvfrom→physical reply observed | 15,749→15,078 | N/A→N/A |
| Framework reply decode→completion queue | 0→0 | 1,680→4,752 |
| binding source recvfrom→binding completion/typed decode | 26,330→22,313 | N/A→N/A |
| Framework completion queue→worker execute | 1,162→1,736 | 0→0 on same-thread samples |
| Framework completion execute→caller/next request | 0→0 | 1,112→4,184 |
| binding completion→caller/next request | 0→0 | 1,112→4,184 |

## 5. 무추적 결과와 중복 작업

무추적 실행은 condition B와 같은 CPU pinning에서 trace instrumentation을 끄고 RUNS=3으로 수행했다. 각 run은 20초 warmup과 5초 측정이었다. 아래 µs는 각 run의 measured latency mean 세 개의 중앙값이다. 괄호에는 각 run의 처리량을 적었다.

| payload | binding µs/request, RUNS=3 | Framework µs/request, RUNS=3 |
|---|---:|---:|
| 1KiB | 87.952 (11,047.2 / 12,121.2 / 11,121.4 req/s) | 145.032 (6,719.4 / 6,784.6 / 6,836.2 req/s) |
| 4KiB | 91.961 (10,359.4 / 10,234.4 / 11,168.8 req/s) | 151.809 (6,031.2 / 6,383.4 / 6,367.6 req/s) |

무추적 중앙값의 Framework/binding 비율은 1KiB에서 1.649, 4KiB에서 1.651이다. 두 값 모두 예산 경계인 1/0.6=1.667 아래이며, µs 여유는 각각 약 1.6µs와 1.5µs다. 처음 제공된 155.7/161.9µs는 이번 condition B RUNS=3에서 재현되지 않았다.

Framework 경로에서 같은 의미의 중복 검증이나 payload encode는 발견하지 못했다. 하지만 본문은 client/target 직렬화 후 service multipart envelope를 만들 때 한 번 더 복사된다. 위치는 `ZLinkManagedMeshNode.cs:10917-10943`, `ZLinkApplicationPayloadEnvelopeCodec.cs:153-163, 388-410`이다. 이는 service wire framing을 위한 별도 copy이며 같은 check나 같은 encode를 두 번 수행하는 경로는 아니다. multipart receive는 view를 쓰므로 수신 framing copy는 없다. 이번 요청 경로에서 수정은 하지 않았다.
