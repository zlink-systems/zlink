# .NET #4350 L0 fresh 재측정 및 #4351 L1 permit

## L0 fresh (Release, msg/s 중앙값)

| 셀 | 반복 | L0 | Binding | L0/Binding | G5 spread L0/Binding | 판정 | Raw |
|---|---:|---:|---:|---:|---:|---|---|
| send-saturation 1 KiB | 3 | 441,115.8 | 436,655.8 | 1.0102 | 5.3% / 4.7% | 통과 | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-send-saturation-1024/results.json` |
| send-saturation 4 KiB | 3 | 177,824.4 | 175,044.2 | 1.0159 | 4.5% / 3.8% | 통과 | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-send-saturation-4096/results.json` |
| request-serial 1 KiB | 5 | 11,011.0 | 10,266.6 | 1.0725 | 6.2% / 17.9% (G5 실패) | 비율 통과 | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-serial-1024/results.json`; `raw/l0-five-run-extra/run{1,2}/{zlink-framework-dotnet,zlink-dotnet}-request-serial-1024/results.json` |
| request-serial 4 KiB | 5 | 9,529.2 | 9,447.4 | 1.0087 | 39.1% (G5 실패) / 8.4% | 비율 통과 | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-serial-4096/results.json`; `raw/l0-five-run-extra/run{1,2}/{zlink-framework-dotnet,zlink-dotnet}-request-serial-4096/results.json` |
| request-backpressure 1 KiB | 3 | 102,862.4 | 103,203.4 | 0.9967 | 3.1% / 3.3% | 통과 | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-backpressure-1024/results.json` |
| request-backpressure 4 KiB | 3 | 97,983.6 | 101,483.2 | 0.9655 | 5.1% / 1.7% | 통과 | `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-backpressure-4096/results.json` |

L0 판정 기준은 각 셀 L0/Binding ≥ 0.95다. 여섯 셀 모두 비율 기준을 통과했다. 모든 raw 셀은 errors=0, server_errors=0, submitted=completed=target_stats.received를 만족한다. request-serial 두 셀은 지정된 5회 뒤에도 G5 spread가 10%를 넘는 행이 있다.

## L1 단계 결과

L1 Release 빌드와 6셀 측정은 실행하지 못했다. 4시간 상한(12:00:40 UTC)에 도달할 때 공유 측정 잠금은 선행 C++ 실험이 소유하고 있었고, 그 뒤로도 Java 실험이 대기 중이었다. DLL freshness 검증 및 L1 측정이 남아 있다. 아래 표와 손실 판정은 측정되지 않아 비어 있다. 단계 손실 기준은 `1 − (L1/Binding)/(L0/Binding)`이며 통과 한도는 7%다.

| 셀 | 반복 | L1 중앙값 | Binding 중앙값 | L1/Binding | L0/Binding | 단계 손실 | 판정 | Raw |
|---|---:|---:|---:|---:|---:|---:|---|---|
| send-saturation 1 KiB | — | — | — | — | 1.0102 | — | — | L0: `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-send-saturation-1024/results.json` |
| send-saturation 4 KiB | — | — | — | — | 1.0159 | — | — | L0: `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-send-saturation-4096/results.json` |
| request-serial 1 KiB | — | — | — | — | 1.0725 | — | — | L0: `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-serial-1024/results.json`; `raw/l0-five-run-extra/run{1,2}/{zlink-framework-dotnet,zlink-dotnet}-request-serial-1024/results.json` |
| request-serial 4 KiB | — | — | — | — | 1.0087 | — | — | L0: `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-serial-4096/results.json`; `raw/l0-five-run-extra/run{1,2}/{zlink-framework-dotnet,zlink-dotnet}-request-serial-4096/results.json` |
| request-backpressure 1 KiB | — | — | — | — | 0.9967 | — | — | L0: `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-backpressure-1024/results.json` |
| request-backpressure 4 KiB | — | — | — | — | 0.9655 | — | — | L0: `raw/l0-fresh-corrected/run{1,2,3}/{zlink-framework-dotnet,zlink-dotnet}-request-backpressure-4096/results.json` |

## 복원 코드와 커밋

- L0 기준 commit: `a542ee23843314eae7b03736e689041e1a5310a5`.
- L1 permit commit: `b6f58168a6e3d2499138ea36f9958d8a7fd7a3ef`, 부모는 L0 기준 commit이다. 로컬 커밋이며 push/PR은 없다.
- Queue capacity와 생성: `framework/bench/grpc/dotnet/ZLinkServer/Program.cs:52-63`; permit은 receive 전 `:162-166`, handler 첫 호출 직전 반환은 request `:169-172`, send `:177-179`, no-data/error 반환은 `:182-202`다.
- 사용한 Framework 내부 구현은 capacity resolver `framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs:275-322`, queue release `:520-534`, lease `:1177-1204`다. L1에서만 `WithGrpcBench.ZLinkServer` internal visibility를 부여한다: `framework/languages/dotnet/Directory.Build.props:4`, `framework/languages/dotnet/src/Zlink.Framework/Properties/AssemblyInfo.cs:10-12`.
- L1에서도 L0의 100 ms blocking 단건 `Recv(RecvFlags.None)`, no-drain 동작, reply용 새 body encode/header 및 2-part submit을 유지했다.

## L1 비용 분석·제안

L1 측정이 없어 7% 손실 판정, 비용 프로파일 및 최적화 제안은 없다. 최적화 코드는 구현하지 않았다.

## 상한 시점

12:00:40 UTC에 측정을 중단했다. L0 6셀 결과와 L1 permit 복원 commit은 완료했으며, L1 commit의 Release 빌드·6셀 측정은 미완료다. cross test는 실행하지 않았다.
