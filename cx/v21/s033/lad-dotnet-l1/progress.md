# .NET L0 재측정 및 L1 진행

## L0 receive/reply parity — 측정 전 기록

| 작업 | Binding raw server | Framework L0 server | 비교 |
|---|---|---|---|
| wait/loop | `framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs:26-28,74-80`: 100 ms receive timeout, 기본 blocking `Recv`, record 한 건 처리 후 blocking receive | `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:25-26,87-105`: 같은 100 ms timeout, `RecvFlags.None`, record 한 건 처리 후 다음 receive | 일치 |
| drain | 없음 | 없음 | 일치 |
| parse/handler | `ZLinkRawServer/Program.cs:74-80`: `PayloadPart`, `RawWire.Decode`/처리 | `ZLinkServer/Program.cs:101-102,174-175`: payload part 후 고정 L0 handler의 직접 호출 및 protobuf parse | L0의 고정 handler 호출만 추가 |
| metrics | `BenchServerMetrics.RecordReceived/Record`; 메시지당 같은 lock/counter 경로 | 같은 metrics 경로 | 추가 atomic 없음 |
| reply | `ZLinkRawServer/Program.cs:74-80,103-108`: 새 payload wrapper, 새 protobuf body encode 1회, 새 header, 2-part submit | `ZLinkServer/Program.cs:101-107,174-185`: 같은 wrapper/body encode/header/2-part submit | 일치 |
| 대기·할당·복사 | 메시지당 blocking receive 1회, reply일 때 body/header 신규 할당, protobuf parse/encode, 추가 drain 없음 | 같은 대기/할당/parse/encode/copy, L0 handler 직접 호출 1회 추가 | receive·reply 부가 비용 없음 |

L0 처리량 측정은 위 parity를 전제로 진행한다. L0는 raw와 같은 수신 대기, drain, metrics, 새 body/header 생성과 submit 경로를 사용한다.

## L0 measurement preflight

- Commit: `a542ee23843314eae7b03736e689041e1a5310a5`; dirty: `no`; merge-base with origin/main: `494c5f4827db8d86a5dce716b7a8da6e7e9cf970`.
- Build: Release solution build succeeded (0 errors, 25 warnings), `raw-build-l0.log`.
- DLLs (all from this worktree; each newer than its corresponding source): Framework server `framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll`, SHA-256 `a7ec6164705a035eba6def37ba741d713f9fd3966c63ab06bc0c0df73203bf72`; client `framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll`, SHA-256 `72dec2e98a1774ec618d4cbbd7fa907ca132b8b4e10ff911f03e6106699c7820`; binding server `framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll`, SHA-256 `1039fd675820952691481dbaf68b49c550ec695543c3e8ff678bca64c741b67a`.
- mtime check (source -> DLL): ZLinkServer.cs `1794547648 -> 1794556832`; Client.cs `1794483108 -> 1794556839`; ZLinkRawServer.cs `1794465423 -> 1794556836`.
- Parity checked for this measurement: raw receive `ZLinkRawServer/Program.cs:26-28,74-80`; L0 receive `ZLinkServer/Program.cs:25-26,87-105`; both 100 ms blocking one-message receive, no drain, same metrics and no added atomics. Reply raw `:74-80,103-108`; L0 `ZLinkServer/Program.cs:101-107,174-185`; both allocate and encode one new body and one header, submit two parts; same parse/copy behavior. Per-message waits: one blocking receive in each; allocations: same request/reply objects; atomics: none extra; copies: no forwarded/reused request body, same protobuf decode/encode.
- Planned locked session: six cells (send-saturation, request-serial, request-backpressure × 1024/4096), 3 runs each; source CPUs 0-7, target CPUs 8-15, warmup 30s / active 5s. Output: `raw/l0-fresh`.

## Discarded initial L0 attempt

- `raw/l0-fresh/run1/zlink-framework-dotnet-send-saturation-1024/` is retained but excluded: `run-six.sh` initially omitted `ZLINK_DOTNET_LADDER_CLIENT=raw`, so the regular Framework client sent startup/protocol frames to the binding-shaped L0 receiver. Target log records protobuf invalid-tag errors and the source stats endpoint did not become ready. The corrected runner now selects the established L0 raw client transport; the six-cell retry will use `raw/l0-fresh-corrected/` after the exclusive lock becomes available.
- l0-fresh-corrected cell=send-saturation-1024 HEAD=a542ee23843314eae7b03736e689041e1a5310a5 dirty=no merge-base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970 DLLs=server=a7ec6164705a035eba6def37ba741d713f9fd3966c63ab06bc0c0df73203bf72;client=72dec2e98a1774ec618d4cbbd7fa907ca132b8b4e10ff911f03e6106699c7820;binding=1039fd675820952691481dbaf68b49c550ec695543c3e8ff678bca64c741b67a
- l0-fresh-corrected cell=send-saturation-4096 HEAD=a542ee23843314eae7b03736e689041e1a5310a5 dirty=no merge-base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970 DLLs=server=a7ec6164705a035eba6def37ba741d713f9fd3966c63ab06bc0c0df73203bf72;client=72dec2e98a1774ec618d4cbbd7fa907ca132b8b4e10ff911f03e6106699c7820;binding=1039fd675820952691481dbaf68b49c550ec695543c3e8ff678bca64c741b67a
- l0-fresh-corrected cell=request-serial-1024 HEAD=a542ee23843314eae7b03736e689041e1a5310a5 dirty=no merge-base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970 DLLs=server=a7ec6164705a035eba6def37ba741d713f9fd3966c63ab06bc0c0df73203bf72;client=72dec2e98a1774ec618d4cbbd7fa907ca132b8b4e10ff911f03e6106699c7820;binding=1039fd675820952691481dbaf68b49c550ec695543c3e8ff678bca64c741b67a
- l0-fresh-corrected cell=request-serial-4096 HEAD=a542ee23843314eae7b03736e689041e1a5310a5 dirty=no merge-base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970 DLLs=server=a7ec6164705a035eba6def37ba741d713f9fd3966c63ab06bc0c0df73203bf72;client=72dec2e98a1774ec618d4cbbd7fa907ca132b8b4e10ff911f03e6106699c7820;binding=1039fd675820952691481dbaf68b49c550ec695543c3e8ff678bca64c741b67a
- l0-fresh-corrected cell=request-backpressure-1024 HEAD=a542ee23843314eae7b03736e689041e1a5310a5 dirty=no merge-base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970 DLLs=server=a7ec6164705a035eba6def37ba741d713f9fd3966c63ab06bc0c0df73203bf72;client=72dec2e98a1774ec618d4cbbd7fa907ca132b8b4e10ff911f03e6106699c7820;binding=1039fd675820952691481dbaf68b49c550ec695543c3e8ff678bca64c741b67a
- l0-fresh-corrected cell=request-backpressure-4096 HEAD=a542ee23843314eae7b03736e689041e1a5310a5 dirty=no merge-base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970 DLLs=server=a7ec6164705a035eba6def37ba741d713f9fd3966c63ab06bc0c0df73203bf72;client=72dec2e98a1774ec618d4cbbd7fa907ca132b8b4e10ff911f03e6106699c7820;binding=1039fd675820952691481dbaf68b49c550ec695543c3e8ff678bca64c741b67a

## 첫 유효 L0 측정값

- `send-saturation × 1 KiB`, Framework L0 run1: `455,894.6 msg/s`, `errors=0`, `server_errors=0`, `submitted=completed=received=2,279,473`; 원자료 `raw/l0-fresh-corrected/run1/zlink-framework-dotnet-send-saturation-1024/results.json`. 이 값은 해당 cell의 중앙값이 아니며 나머지 세션이 끝난 뒤 계산한다.

## L0 fresh 결과 (serial 두 셀 5회)

단위 msg/s. 중앙값은 `results.json`의 `throughput_per_second`를 계산했다. G5 spread는 median에서 가장 먼 run 차이 ÷ median이며 한도는 10%다. 각 행의 두 raw 경로는 Framework와 binding의 run 결과를 포함한다.

| 셀 | 반복 | L0 중앙값 | Binding 중앙값 | L0/Binding | L0 G5 | Binding G5 | Raw |
|---|---:|---:|---:|---:|---:|---:|---|
| send-saturation × 1 KiB | 3 | 441,115.8 | 436,655.8 | 1.0102 | 5.3% | 4.7% | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-send-saturation-1024/results.json` |
| send-saturation × 4 KiB | 3 | 177,824.4 | 175,044.2 | 1.0159 | 4.5% | 3.8% | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-send-saturation-4096/results.json` |
| request-serial × 1 KiB | 5 | 11,011.0 | 10,266.6 | 1.0725 | 6.2% | 17.9% (실패) | `raw/l0-fresh-corrected/run{1,2,3}/...request-serial-1024/results.json`; `raw/l0-five-run-extra/run{1,2}/...request-serial-1024/results.json` |
| request-serial × 4 KiB | 5 | 9,529.2 | 9,447.4 | 1.0087 | 39.1% (실패) | 8.4% | `raw/l0-fresh-corrected/run{1,2,3}/...request-serial-4096/results.json`; `raw/l0-five-run-extra/run{1,2}/...request-serial-4096/results.json` |
| request-backpressure × 1 KiB | 3 | 102,862.4 | 103,203.4 | 0.9967 | 3.1% | 3.3% | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-backpressure-1024/results.json` |
| request-backpressure × 4 KiB | 3 | 97,983.6 | 101,483.2 | 0.9655 | 5.1% | 1.7% | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-backpressure-4096/results.json` |

모든 결과는 errors/server_errors 0이며 submitted=completed=target_stats.received다. 다섯 회 측정 뒤에도 request-serial 1 KiB binding 및 request-serial 4 KiB L0 spread가 G5 한도 10%를 넘는다. L0 성능 비율은 여섯 셀 모두 0.95 이상이다.

## L1 permit 복원

- L1 commit: `b6f58168a6e3d2499138ea36f9958d8a7fd7a3ef` (`perf(dotnet): restore application permit for ladder L1`), 부모 L0 commit `a542ee23843314eae7b03736e689041e1a5310a5`.
- L1 queue 생성/capacity: `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:52-63`; receive 전 원래 Framework permit 획득과 Remote origin: `:162-166`; no-data·오류 반환: `:182-202`; handler 첫 호출 직전 반환: request `:169-172`, send `:177-179`.
- 기존 Framework 구성 요소: capacity resolver `framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:275-322`; queue release `:520-534`; lease `:1177-1204`. L1 compile symbol은 `framework/languages/dotnet/Directory.Build.props:4`에서 Framework와 bench project reference에 전달되어 friend visibility를 `framework/languages/dotnet/src/Zlink.Framework/Properties/AssemblyInfo.cs:10-12`에 조건부로만 부여한다. Bench ladder define은 `framework/bench/grpc/dotnet/Directory.Build.props:7`.
- Receive는 L0와 같은 100 ms timeout/blocking `Recv(RecvFlags.None)` 단건 처리이며 drain은 없다. Reply는 L0와 같은 새 protobuf body/header 및 2-part submit이다. 메시지마다 raw/L0는 blocking `Recv` 1회, L1은 여기에 Framework acquire/release state-lane turn을 더한다. Reply 할당은 둘 다 새 body/header이며, L1은 queue lease와 async acquire/release operation state를 추가한다. Metrics atomic은 동일하고 L1 permit 반환은 lease state exchange를 수행한다. Parse/encode는 각각 1회이며 request body 재사용/추가 복사는 없다. 아직 L1 build 및 fresh measurement는 수행하지 않았다.

## 시간 상한 도달

- 4시간 상한 12:00:40 UTC에 측정을 중단했다. L0 표는 여섯 셀 모두 완성했고, L1 permit 코드는 `b6f58168a6e3d2499138ea36f9958d8a7fd7a3ef`로 커밋했다.
- 상한 시 공유 측정 잠금은 선행 C++ 작업이 소유했고 Java 측정도 대기 중이었다. L1 커밋 Release 빌드, DLL freshness 확인, L1 6셀 측정·단계 손실 판정은 미완료다. raw 파일은 모두 보존했다. Cross test는 실행하지 않았다.
- l0-five-run-extra cell=request-serial-1024 HEAD=a542ee23843314eae7b03736e689041e1a5310a5 dirty=no merge-base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970 DLLs=server=a7ec6164705a035eba6def37ba741d713f9fd3966c63ab06bc0c0df73203bf72;client=72dec2e98a1774ec618d4cbbd7fa907ca132b8b4e10ff911f03e6106699c7820;binding=1039fd675820952691481dbaf68b49c550ec695543c3e8ff678bca64c741b67a
- l0-five-run-extra cell=request-serial-4096 HEAD=a542ee23843314eae7b03736e689041e1a5310a5 dirty=no merge-base=494c5f4827db8d86a5dce716b7a8da6e7e9cf970 DLLs=server=a7ec6164705a035eba6def37ba741d713f9fd3966c63ab06bc0c0df73203bf72;client=72dec2e98a1774ec618d4cbbd7fa907ca132b8b4e10ff911f03e6106699c7820;binding=1039fd675820952691481dbaf68b49c550ec695543c3e8ff678bca64c741b67a
