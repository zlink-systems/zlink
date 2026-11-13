| 하위 구간 | p50 (µs) | p90 (µs) | 평균 (µs) | thread |
|---|---:|---:|---:|---|
| B1 수신 반환→B2 execution pool enqueue 직전 | 1.192 | 1.753 | 1.363 | TID 11 또는 14, .NET Long Running Task |
| B2 enqueue 직전→B3 route dispatch 진입 | 14.427 | 19.145 | 15.140 | TID 11 또는 14, .NET Long Running Task |
| B3 route dispatch 진입→B4 binding reply 제출 | 6.702 | 9.247 | 7.097 | TID 11 또는 14, .NET Long Running Task |
| D1 binding completion 전달→D2 ThreadPool queue | 5.701 | 7.704 | 6.351 | .NET TP Worker, TID 8·19·25·26·28·29; 시작·queue는 요청마다 같은 TID |
| D2 ThreadPool queue→D3 work item 시작 | 2.094 | 2.795 | 1.921 | .NET TP Worker, 주로 TID 8·26·28·29·25·19; queue와 실행 TID가 바뀌는 요청도 있음 |
| D3 work item 시작→D4 다음 요청 binding 제출 | 4.037 | 5.851 | 4.503 | .NET TP Worker, 주로 TID 8·26·28·29·25·19 |

## ThreadPool 표본

Source 프로세스에서 전용 sampler thread가 100ms마다 표본을 기록했습니다. 활성 10초 동안 표본은 100개였습니다. `ThreadCount`는 최소 6, 중앙값 6, 90백분위 6, 평균 6.0, 최대 7이었습니다. `PendingWorkItemCount`는 최소 0, 중앙값 0, 90백분위 0, 평균 0.08, 최대 1이었습니다. 누적 `CompletedWorkItemCount`는 최소 228,532, 중앙값 286,862, 90백분위 336,132, 평균 287,249, 최대 348,755였습니다.

개별 D2→D3 대기 구간 안에 들어온 100ms 표본은 0개였습니다. D2 이후 100ms 이내에 다음 표본이 있는 59,637개 대기에서 가장 가까운 표본의 `ThreadCount`는 중앙값 6, 평균 6.02였습니다. `PendingWorkItemCount`는 중앙값 0, 평균 0.08, 최대 1이었습니다. 이 표본 간격은 약 2.1µs인 D2→D3 대기 자체를 직접 관찰하기에는 거칩니다.

## 처리량

계측을 켠 Framework 실행은 6,018.2 req/s였습니다. 계측을 끈 Framework 실행은 6,135.5 req/s였습니다. 같은 한 번 실행에서 계측은 처리량이 117.3 req/s, 약 1.9% 낮았습니다. Binding 실행은 11,989.9 req/s였습니다. 비교 기준으로 전달된 기존 비계측 참고값은 Framework 5,998 req/s, Binding 11,370 req/s였습니다.

## 요청 연결과 syscall 경계 대조

활성 10초 창에서 B1–B4는 60,153개 요청, D1–D4는 60,152개 요청을 연결했습니다. B는 수신 시각의 순번과 request-serial의 단일 in-flight 순서로 묶었습니다. D1은 binding `CompletionOwner`를 수정하지 않고, 그 completion task를 기다리는 `ObserveNativeDeadlineReplyAsync`에서 `await physical`이 반환된 직후 기록한 첫 Framework 관측 시각입니다. D1–D3는 source 프로세스 순번을 사용했습니다. 첫 벤치 요청보다 앞선 source 초기 completion 하나를 제외한 뒤 D3 시각과 같은 순번의 D4 다음 제출을 연결했습니다.

B1→B4 전체는 p50 22.541µs, p90 29.734µs, 평균 23.600µs였습니다. nseg2의 syscall 경계 B p50/p90 106.7/147.0µs보다 각각 84.2/117.3µs 짧습니다. D1→D4 전체는 p50 11.761µs, p90 15.749µs, 평균 12.774µs였습니다. nseg2의 syscall 경계 D p50/p90 126.7/172.9µs보다 각각 114.9/157.2µs 짧습니다. 두 관리 코드 span은 syscall 경계 값과 가깝지 않습니다. B4와 D4는 binding 호출 직전이고, nseg2는 Core의 소켓 syscall까지 포함하므로 이 측정에서 binding/Core 제출과 syscall 경계 사이 시간은 빠져 있습니다.

B의 네 표식 모두 활성 요청에서 같은 Long Running TID 11 또는 14에 남았습니다. 따라서 이 실행의 B2→B3 지연은 OS thread ID 교체 시간으로 해석할 수 없습니다. D1/D2는 각 요청에서 같은 ThreadPool worker였고, D2→D3에서 worker ID가 유지되거나 다른 ThreadPool worker로 바뀌었습니다. 기록된 이름은 모두 `.NET TP Worker`였습니다.

## 빌드와 변경 파일

`flock -s /root/jobs/fx/cx/measure.lock taskset -c 0-7 bash dotnet/build.sh`가 통과했습니다. 기존 nullable, XML 문서 및 중복 import 경고가 있었고 빌드 오류는 없었습니다. Framework trace는 2,097,152개 고정 크기 record ring에, source sampler는 별도 고정 ring에 기록하고 프로세스 종료 때 JSONL로 덤프했습니다. `ZLINK_DSEG_TRACE`가 없으면 timestamp와 링 접근은 수행하지 않습니다.

계측 전용 변경이며 커밋이나 PR은 만들지 않았습니다. 리팩토링 점검 결과, 제거할 계측 외 리팩토링 항목은 없습니다. 감독자가 삭제할 소스 변경 파일은 다음 일곱 개입니다.

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkDsegTrace.cs` (신규)
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotReplySubmitter.cs`
- `framework/bench/grpc/dotnet/Client/DsegBenchTrace.cs` (신규)
- `framework/bench/grpc/dotnet/Client/BenchMeasurement.cs`
