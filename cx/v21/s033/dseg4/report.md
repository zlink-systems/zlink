| 연속 구간 | Framework A 이전 p50 / p90 / 평균 (µs) | Framework A p50 / p90 / 평균 (µs) | Binding p50 / p90 / 평균 (µs) | A − Binding p50 / p90 / 평균 (µs) |
|---|---:|---:|---:|---:|
| source sendto → target recvfrom | 28.433 / 36.938 / 29.609 | 30.697 / 38.801 / 31.486 | 28.713 / 35.712 / 36.420 | +1.984 / +3.089 / −4.934 |
| target recvfrom → B1 | 42.067 / 54.412 / 44.148 | 42.633 / 55.856 / 44.653 | 17.583 / 24.613 / 19.258 | +25.050 / +31.243 / +25.395 |
| B1 → B4 | 22.541 / 29.734 / 23.600 | 41.696 / 56.925 / 44.970 | 0.521 / 1.653 / 0.800 | +41.175 / +55.272 / +44.170 |
| B1 → B2 | 1.563 / 2.365 / 1.817 | 1.423 / 2.194 / 1.547 | — | — |
| B2 → B3 | 23.313 / 31.798 / 24.571 | 31.268 / 42.655 / 34.189 | — | — |
| B3 → B4 | 9.197 / 12.192 / 9.544 | 8.836 / 12.924 / 9.234 | — | — |
| B4 → target sendto | 23.113 / 31.598 / 25.895 | 19.946 / 28.816 / 21.426 | 16.671 / 22.782 / 17.945 | +3.275 / +6.034 / +3.481 |
| target sendto → source recvfrom | 37.589 / 49.020 / 39.240 | 39.412 / 49.731 / 40.135 | 28.633 / 35.876 / 29.964 | +10.779 / +13.855 / +10.171 |
| source recvfrom → D1 | 68.175 / 86.132 / 70.976 | 71.707 / 88.629 / 72.663 | 34.182 / 42.989 / 35.765 | +37.525 / +45.640 / +36.898 |
| D1 → D4 | 11.761 / 15.749 / 12.774 | 11.531 / 18.293 / 13.316 | 2.023 / 4.108 / 2.773 | +9.508 / +14.185 / +10.543 |
| D1 → D2 | 6.261 / 9.089 / 6.687 | 6.782 / 9.578 / 7.809 | 5.701 / 7.704 / 6.351 | +1.081 / +1.874 / +1.458 |
| D2 → D3 | 0.741 / 4.829 / 1.397 | 0.451 / 0.861 / 0.937 | 2.094 / 2.795 / 1.921 | −1.643 / −1.934 / −0.984 |
| D3 → D4 | 3.827 / 5.860 / 4.417 | 4.077 / 5.951 / 4.570 | 4.037 / 5.851 / 4.503 | +0.040 / +0.100 / +0.067 |
| D4 → 다음 source sendto | 31.889 / 41.386 / 34.048 | 33.922 / 41.833 / 35.310 | 16.180 / 21.290 / 17.108 | +17.742 / +20.543 / +18.202 |

Binding 실행에서는 B1→B2, B2→B3, B3→B4를 따로 계측하지 않았다. 따라서 이 세 행의 Binding 값과 차이는 계산하지 않았다. B1→B4와 D1→D4는 각 구간 전체의 관측값을 썼으며, 하위 구간 p50의 합으로 전체 p50을 추정하지 않았다.

Framework A 계측은 commit `060ce2af5d`에서 수행했다. 세션 링 버퍼와 `ZLINK_DSEG_TRACE` 환경 변수 게이트를 사용했다. B1은 `ZLinkManagedMeshNode.ReceiveRawBatch`에서 `_socket.Recv`가 성공해 반환한 시각을 저장하고, NodeRequest로 판별된 `ProcessReceived`에서 그 시각을 기록했다. B2는 reply handler를 만들고 `EnqueueOwned`를 호출하기 직전, B3은 `ZLinkMeshNodeRouteDispatcher.DispatchNodeRouteAsync`의 request 진입, B4는 `CreateNativeReplyHandler`에서 `SubmitNativeTerminalReply` 직전이다. 소스 D1은 `ObserveNativeDeadlineReplyAsync`의 `await physical` 반환 직후다. D2는 `ZLinkCompletionDispatcher.Post`의 queue 호출 직전, D3은 work item의 `Execute` 진입, D4는 다음 요청을 시작하기 직전이다.

세그먼트 시간은 perf의 `CLOCK_MONOTONIC`과 `Stopwatch.GetTimestamp`로 정렬했다. perf는 두 프로세스 PID에 `raw_syscalls:sys_enter`, `raw_syscalls:sys_exit`, `sched:sched_switch`, `sched:sched_wakeup`을 3초간 기록했다. 실제 trace 창은 3.029초였고 완전하고 시간 순서가 맞는 연결 cycle은 9,888개였다. 캡처 PID는 source 3437669, target 3437553이었다.

대상 쪽에서 recvfrom은 `ZLINKbg/IO/0` TID 3437585에서 관측됐다. B1과 B2는 모두 `.NET Long Running Task` managed ID 10에서 기록됐다. B2→B3 9,987개 표본 모두 managed ID 10에서 ID 16으로 바뀌었다. 따라서 route handler는 receive thread가 아니라 별도 Long Running Task worker에서 실행됐다. B3와 B4는 9,987개 모두 ID 16에서 실행됐다. B4 뒤 target sendto는 `ZLINKbg/IO/1` TID 3437586에서 관측됐다.

소스 쪽 source recvfrom은 `ZLINKbg/IO/2` TID 3437695에서 관측됐다. D1과 D2는 모두 `.NET TP Worker`에서 실행됐고 9,988개 중 9,988개 요청에서 같은 managed thread였다. D2→D3에서는 9,298개가 같은 worker였고 690개는 worker가 바뀌었다. D3 뒤 다음 요청의 D4 표식은 9,988개 모두 D3와 같은 worker에서 기록됐다. 즉, 이 실행에서 caller가 다음 요청을 시작한 worker는 dispatcher work item이 끝난 worker와 같았다. D1은 물리 응답 task를 await한 Framework 관측점이다. perf에서 Core recvfrom thread를 확인했지만 binding 내부 completion drain 자체에는 표식이 없으므로 그 내부 drain이 실행된 thread와 D1 worker를 동일한 작업자로 단정하지 않았다.

활성 15초 전체의 Framework A 처리량은 5,022.3 req/s였다. source는 75,336건을 제출하고 모두 완료했으며 오류는 없었다. target도 75,336건을 받았고 오류는 없었다. 비교 기준 처리량은 Framework A 이전 dseg2의 5,360.1 req/s와 Binding dseg3의 9,421.8 req/s다. perf 시작 전 스크립트가 MemAvailable 12 GiB 이상을 확인했고, 캡처 종료 직후 값은 25.7 GiB였다. perf 출력에는 3,965,452개 event 행이 있었고, 1,269개 이벤트를 포함한 lost record 1개와 out-of-order 이벤트 2개가 기록됐다. 벤치 요청 손실은 source 완료 수와 target 수신 수가 같고 오류가 0건인 것으로 확인했다.

A와 Binding의 p50 차이가 가장 큰 두 edge는 B1→B4(+41.175 µs)와 source recvfrom→D1(+37.525 µs)다. B1→B4 중 B2→B3 p50은 31.268 µs였고, 표식은 receive 쪽 managed ID 10에서 route worker ID 16으로 바뀌었다. A 코드는 `ProcessReceived`에서 service application 요청을 판별하고 `MeshReceiveRecord`와 native reply handler를 만든 뒤 `EnqueueOwned`로 넘긴다. `EnqueueOwned`는 application queue와 owned mailbox에 전달하고, `ZLinkMeshDispatchPump.EnsureStartedOnLane`은 application source worker를 만든다. 이후 `DispatchNodeRouteAsync`는 dispatch scope를 만들고 route handler를 찾은 뒤 `_routeInvoker.InvokeRequestAsync`를 호출하고 reply를 보낸다. 이 위치는 각각 `ZLinkManagedMeshNode.cs:7397-7405`, `ZLinkManagedMeshNode.cs:12936-12984`, `ZLinkMeshDispatchPump.cs:103-127`, `ZLinkMeshNodeRouteDispatcher.cs:624-645`, `ZLinkMeshNodeRouteDispatcher.cs:711-757`이다. Binding bench의 target handler는 `ZLinkRawServer/Program.cs:75-83`에서 body를 decode하고 metrics를 기록한 다음 reply를 만들어 제출한다. B2→B3 시간은 queue handoff와 dispatch 시작 사이에 관측됐다.

source recvfrom→D1은 source recvfrom 뒤 물리 응답 task가 Framework의 `ObserveNativeDeadlineReplyAsync`에서 재개될 때까지의 경계다. 이 실행에서 D1은 `.NET TP Worker`에서 기록됐고, p50 차이는 37.525 µs였다. Framework A는 `ZLinkManagedMeshNode.cs:11632-11671`에서 `ZLinkDurableRequest.RequestAsync(...).AsTask()`를 `physical`로 넘긴다. ready route의 `ZLinkDurableRequest.RequestAsync`는 남은 deadline을 계산하고 route를 확인한 뒤 submit task를 await해 반환한다(`ZLinkDurableRequest.cs:8-17`, `ZLinkDurableRequest.cs:107-109`). 따라서 A 경로에는 이 Framework async wrapper가 D1 전의 코드 경로에 있다. Binding bench의 `RawBenchTransport.RequestAsync`는 `SubmitRequest(...).Reply`를 직접 await한다(`Client/Program.cs:428-435`). D1 직후에는 Framework의 reply decode와 `CompleteManagedOperation`이 이어지고, completion work item은 `ZLinkCompletionDispatcher.Post`를 거쳐 worker에서 caller task를 완료한다(`ZLinkManagedMeshNode.cs:11180-11202`, `ZLinkManagedMeshNode.cs:11156-11162`, `ZLinkCompletionDispatcher.cs:13-38`). 캡처는 binding completion drain과 Framework wrapper의 각 내부 지점에 별도 표식을 두지 않았으므로 37.525 µs를 Framework wrapper와 binding completion으로 나누어 귀속할 수 없다. 이 값은 syscall에서 D1까지의 전체 관측 경계다.

빌드는 `/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash dotnet/build.sh`로 통과했다. 첫 빌드 시 client trace 파일이 컴파일 입력 위치에 없어 실패했으며, 파일을 지정 worktree의 Client 디렉터리에 둔 뒤 같은 빌드 명령이 통과했다. 벤치와 perf는 종료 코드 0이었다. 별도 test는 실행하지 않았다.

계측 때문에 수정한 저장소 파일은 다음과 같다.

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkDsegTrace.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs`
- `framework/bench/grpc/dotnet/Client/DsegBenchTrace.cs`
- `framework/bench/grpc/dotnet/Client/BenchMeasurement.cs`
- `framework/bench/grpc/dotnet/Client/Program.cs`
- `framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs`
- `framework/bench/grpc/dotnet/ZLinkRawServer/WithGrpcBench.ZLinkRawServer.csproj`

측정용 보조 스크립트와 보고서는 `/root/jobs/fx/cx/v21/s033/dseg4/capture-a.py`, `/root/jobs/fx/cx/v21/s033/dseg4/analyze-a.py`, `/root/jobs/fx/cx/v21/s033/dseg4/report.md`다. perf trace, 실행 로그와 JSON 산출물도 같은 폴더 아래에 뒀다. run script의 기존 CPU pin 변경은 수정하지 않았다. commit은 만들지 않았다.
