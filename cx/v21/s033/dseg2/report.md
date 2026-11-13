# .NET request-serial 1KiB Core↔managed 경계 측정

| 연속 구간 | p50 (µs) | p90 (µs) | 평균 (µs) | thread |
|---|---:|---:|---:|---|
| source Core sendto → target Core recvfrom | 28.433 | 36.938 | 29.609 | source ZLINKbg/IO/0 TID 3087383 → target ZLINKbg/IO/0 TID 3087275 |
| target Core recvfrom → B1 | 42.067 | 54.412 | 44.148 | target ZLINKbg/IO/0 TID 3087275 → .NET Long Running Task managed ID 16/17 |
| B1 → B2 | 1.563 | 2.365 | 1.817 | .NET Long Running Task managed ID 16/17 |
| B2 → B3 | 23.313 | 31.798 | 24.571 | .NET Long Running Task managed ID 16/17 |
| B3 → B4 | 9.197 | 12.192 | 9.544 | .NET Long Running Task managed ID 16/17 |
| B4 → target Core sendto | 23.113 | 31.598 | 25.895 | .NET Long Running Task managed ID 16/17 → target ZLINKbg/IO/1 TID 3087276 |
| target Core sendto → source Core recvfrom | 37.589 | 49.020 | 39.240 | target ZLINKbg/IO/1 TID 3087276 → source ZLINKbg/IO/2 TID 3087385 |
| source Core recvfrom → D1 | 68.175 | 86.132 | 70.976 | source ZLINKbg/IO/2 TID 3087385 → .NET TP Worker |
| D1 → D2 | 6.261 | 9.089 | 6.687 | .NET TP Worker, 요청별 같은 worker |
| D2 → D3 | 0.741 | 4.829 | 1.397 | .NET TP Worker, 요청에 따라 worker가 바뀔 수 있음 |
| D3 → D4 | 3.827 | 5.860 | 4.417 | .NET TP Worker |
| D4 → 다음 source Core sendto | 31.889 | 41.386 | 34.048 | .NET TP Worker → source ZLINKbg/IO/0 TID 3087383 |

경계 구간은 perf의 CLOCK_MONOTONIC 시각과 Stopwatch.GetTimestamp를 맞춰 요청 순서로 연결했다. 표의 완전한 연속 cycle은 10,359개다. recvfrom 시각은 성공한 syscall의 반환 시각이고 sendto 시각은 syscall 진입 시각이다. 프레임 크기는 요청 방향에서 주로 1,425바이트, 응답 방향에서 1,390바이트였다.

target recvfrom → B1에는 Core 수신 뒤 managed Long Running thread가 record를 얻기까지의 전달·깨우기·수신 호출 시간이 들어간다. B4 → target sendto에는 binding 제출 뒤 Core 송신 IO가 syscall에 들어가기까지가 들어간다. D4 → 다음 source sendto도 managed 제출 뒤 source Core 송신 IO까지의 시간이다. 이 경계들은 표의 syscall 및 managed 표식으로만 나눴으며, 중간 binding 내부 구간을 별도 계측하지 않았다.

## B2→B3 내부 관측

아래 표는 추가 마커를 켠 같은 active perf 창의 10,447개 B2→B3 구간이다. 각 행은 바로 앞뒤 마커 사이 시간이다. 첫 표는 모든 syscall 경계가 갖춰진 10,359개 complete cycle만 사용했으므로 B2→B3 표본 수와 p50이 조금 다르다.

| 마커 구간 | 의미 | p50 (µs) | p90 (µs) | 평균 (µs) |
|---|---|---:|---:|---:|
| B2 → 20 | B2 이후 다음 ReceiveRawBatch 수신 시도 전 | 2.003 | 3.296 | 2.358 |
| 20 → 22 | Recv(DontWait) 시도. 이번 표본에서는 반환값 false | 0.842 | 1.432 | 1.065 |
| 22 → 53 | DrainRawSocket 반환까지 | 0.692 | 0.972 | 0.824 |
| 53 → 54 | ProcessInfrastructure 진입 전 | 0.060 | 0.090 | 0.136 |
| 54 → 55 | ProcessInfrastructure 실행 | 0.921 | 1.283 | 1.090 |
| 55 → 50 | 다음 WaitAsync 호출 전 | 0.200 | 0.271 | 0.275 |
| 50 → 36 | WaitAsync 경로에서 continuation 처리 뒤까지 | 0.681 | 0.952 | 0.771 |
| 36 → 37 | ReceiveUntilHandoff의 ready application 선택 및 handoff | 5.901 | 7.915 | 6.250 |
| 37 → 40 | application turn 실행 직전까지 | 0.160 | 0.250 | 0.222 |
| 40 → B3 | application.Run 진입부터 route dispatch 표식까지 | 10.970 | 14.637 | 11.540 |

마커 순서는 모든 10,447개 표본에서 `B2 → 20 → 22 → 53 → 54 → 55 → 50 → 36 → 37 → 40 → B3`였다. 20은 `ReceiveRawBatch`에서 `_socket.Recv(..., RecvFlags.DontWait)` 호출 직전이고, 22는 그 호출이 false를 반환한 지점이다. 이 결과는 managed Recv가 다음 record를 얻지 못해 batch를 끝낸 것을 뜻한다. 이 구간에 같은 thread의 Core recvfrom syscall은 관측되지 않았다.

Perf가 식별한 두 native Long Running thread는 TID 3087303과 3087304였고, 각각 managed ID 16과 17에 대응했다. B2→B3 구간 10,447개 중 10,371개에는 같은 native thread에서 syscall 한 번, 70개에는 syscall이 없었고, 나머지 6개에는 여러 syscall이 있었다. 구간 안 syscall은 futex(NR 202, op 0x81, 반환값 1)가 10,381회, futex 반환값 0이 2회, mprotect(NR 10)가 3회, madvise(NR 28)가 3회였다. 따라서 이 구간의 대표 syscall은 futex wake이며, poll·read(eventfd) 호출은 구간 안에 잡히지 않았다.

같은 3초 perf 창 전체에서는 두 Long Running thread가 poll(NR 7, 반환값 1) 10,813회, 8바이트 read 20,306회, read의 EAGAIN 반환 48,974회, futex 반환값 0 20,754회와 1 10,391회를 보였다. 8바이트 read는 fd c9/cf에서 관측됐다. 이 poll·read 재시도와 eventfd 형태의 read는 B2→B3 구간 밖에서 주로 일어났다. managed 표식에서는 매 요청마다 false Recv 시도, DrainRawSocket 반환, ProcessInfrastructure, WaitAsync, ReceiveUntilHandoff continuation/handoff, application.Run과 route dispatch 순서를 확인했다. poll과 eventfd 동작을 각 요청의 B2→B3에 넣어 설명할 근거는 없었다.

이 측정의 B2→B3 p50은 23.313µs다. 앞선 계측 결과의 14.427µs보다 길다. 이번에는 B2부터 B3까지 반복 구간마다 고정 링에 추가 표식을 기록했으므로, 이 수치는 추가 진단 계측의 영향을 포함하며 원래 경로의 무계측 지연으로 읽을 수 없다. 내부 구간별 시간은 표식 사이의 관측 시간이며 표식 기록 자체의 비용도 포함한다.

## D1 앞의 binding 경계

D1은 `ObserveNativeDeadlineReplyAsync`에서 `await physical`이 반환된 직후 기록했다. binding 코드는 수정하지 않았고, `CompletionOwner.Drain` 또는 completion task 완료 시점에는 managed 쪽에서 넣을 수 있는 공용 표식이 없었다. 따라서 source recvfrom → D1 68.175µs p50 구간은 binding의 `CompletionOwner.Drain`, binding request completion/task 완료, continuation 예약과 managed `await physical` 재개까지 포함한다. 이 구간 안에서 각 항목의 시간을 따로 나누지는 못했다.

## 실행, 처리량, 기록 품질

지정된 `dotnet/build.sh`가 통과했다. 두 번째 캡처의 perf와 benchmark도 종료 코드 0이었다. 전체 benchmark active 15초 처리량은 5,360.067 req/s였고, 평균 latency는 0.184ms였다. perf는 active 구간에서 3초만 기록했다. 기록 파일은 4,326,764개 이벤트를 처리했고 2개 lost record에 4,955개 raw event 손실이 보고됐다. `PERF_RECORD_LOST_SAMPLES` 합계도 4,955였으며 이를 별도 손실로 더하지 않았다. 손실이 있어 syscall별 횟수는 일부 과소 집계될 수 있다. perf 창의 요청별 정렬에는 10,359개 완전 cycle을 사용했다.

첫 캡처는 반복 poll-loop 표식이 warmup 중 고정 링을 채워 active B 표식 일부가 남지 않았다. 이를 확인한 뒤 표식을 B2부터 B3까지에만 기록하도록 제한하고 다시 빌드·측정했다. 보고서의 수치는 링이 perf 창을 덮은 두 번째 캡처에서 얻었다.

## 계측 파일과 점검

계측은 측정 전용이며 commit은 만들지 않았다. 현재 worktree에 남긴 source 계측 파일은 다음 여덟 개다.

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkDsegTrace.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkProcessExecutionPool.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotReplySubmitter.cs`
- `framework/bench/grpc/dotnet/Client/DsegBenchTrace.cs`
- `framework/bench/grpc/dotnet/Client/BenchMeasurement.cs`

이번 차례에 추가로 수정한 source 파일은 `ZLinkDsegTrace.cs`, `ZLinkProcessExecutionPool.cs`, `ZLinkManagedMeshNode.cs`다. 분석 보조 스크립트와 raw perf/trace 파일도 `/root/jobs/fx/cx/v21/s033/dseg2/`에 두었다. 리팩토링 점검: 성능 계측 외 발견 0건, POSDDD 발견 0건, 불필요 코드 발견 0건. 계측은 결과 검토 뒤 제거할 항목이다.
