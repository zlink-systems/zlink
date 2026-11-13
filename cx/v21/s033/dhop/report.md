| payload | B1→B2 p50/p90/평균 (µs) | B2→B3 p50/p90/평균 (µs) | B3→B4 p50/p90/평균 (µs) | B2→B5 queue/wake p50/p90/평균 (µs) | B5→B3 dispatch work p50/p90/평균 (µs) | B2→B3 thread 변경 | 왕복 gate on (µs, req/s) | 왕복 gate off (µs, req/s) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1024 B | 0.832 / 1.392 / 1.163 | 15.108 / 22.010 / 16.698 | 5.820 / 9.187 / 6.545 | 10.799 / 15.508 / 11.938 | 4.107 / 6.632 / 4.761 | 98,827 / 98,827 (100%) | 150.34 (6,651.8) | 155.66 (6,424.2) |
| 4096 B | 0.942 / 1.483 / 1.125 | 15.619 / 21.980 / 16.879 | 7.033 / 11.101 / 7.981 | 11.180 / 15.519 / 12.033 | 4.318 / 6.662 / 4.846 | 91,766 / 91,766 (100%) | 163.97 (6,098.6) | 162.93 (6,137.6) |

왕복 시간은 세 run의 중앙 처리량에 1,000,000을 나눈 값이다. 처리량 run별 값은 1024 B gate on 6,697.4, 6,439.6, 6,651.8 req/s, gate off 6,366.0, 6,424.2, 6,512.8 req/s였다. 4096 B gate on은 6,098.6, 6,200.4, 6,072.2 req/s, gate off는 6,042.8, 6,208.0, 6,137.6 req/s였다.

조건 B에서 source는 CPU 0–7, target은 CPU 8–15에 고정했다. 각 크기와 gate 상태별로 RUNS=3, warmup 2초와 active 5초를 실행했다. Gate on과 off는 같은 최종 빌드로 실행했다. 12개 셀 모두 완료 수와 target 수신 수가 일치했고 errors와 server_errors는 0이었다. 구간 통계는 active 5초 내에 모든 표식이 연결된 요청을 합산했으며, p50과 p90은 이전 계측 분석과 같이 선형 보간했다. 표식 표본은 1024 B 98,827건, 4096 B 91,766건이다.

B1은 [ZLinkManagedMeshNode.cs](/root/worktree/zlink-dhop/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:6351)의 성공한 Recv 직후 Stopwatch 시각을 저장하고, NodeRequest 분류 시 7398행에서 그 시각을 기록한다. B2는 같은 파일 7404행에서 EnqueueOwned 호출 직전이다. B3은 [ZLinkMeshNodeRouteDispatcher.cs](/root/worktree/zlink-dhop/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:634)의 route dispatch 진입이다. B4는 ZLinkManagedMeshNode.cs 7460행에서 native reply submit 직전이다. 구간 분리를 위한 B5 보조 표식은 [ZLinkMeshDispatchPump.cs](/root/worktree/zlink-dhop/framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:510)에서 application owner claim의 ReceiveAsync가 성공한 직후다. 표식은 ZLINK_DSEG_TRACE 환경 변수 gate와 2,097,152개 고정 ring buffer를 사용했다.

B1과 B2는 모두 managed thread ID 10의 .NET Long Running Task에서 실행됐다. B3은 route worker인 .NET Long Running Task에서 실행됐고 B2→B3은 모든 표본에서 thread가 바뀌었다. B5와 B3은 모든 표본에서 같은 worker thread였다. B4는 B3과 같은 thread에서 실행됐으며 예외는 1024 B에서 98,827건 중 2건뿐이고 4096 B에서는 전부 같았다.

B2→B3에는 queue wake와 worker scheduling뿐 아니라 claim 수신 뒤 route dispatch 진입까지의 작업이 들어 있다. B5가 수신 완료 시각을 나누므로 B5→B3의 중앙값 4.1–4.3 µs를 dispatch work로 관측했고, B2→B5의 중앙값 10.8–11.2 µs를 queue 대기·wake와 claim 수신 비용으로 관측했다. 요청을 받은 thread에서 이어 처리해 이 구간을 없앤다고 가정할 때 serial 왕복에서 절약될 시간은 대략 11 µs로 추정한다. B2→B5는 순수한 잠든 시간만 재지 않으며, queue wake/scheduling과 claim 수신 경로를 함께 포함한다. 따라서 11 µs는 새 binding API의 직접 A/B 측정값이 아니라 현재 구간을 바탕으로 한 추정치다.

Gate on/off 왕복 중앙값 차이는 1024 B에서 on이 5.33 µs 짧고 4096 B에서 1.04 µs 길었다. 각 크기에서 세 run의 처리량 범위가 겹쳐 이 차이로 표식 overhead를 분리해 말할 수 없다. 표식은 active 처리량에 안정적인 저하를 보이지 않았다.

요청한 빌드 명령은 통과했다. 기존 duplicate import와 nullable/XML 문서 경고가 출력됐지만 빌드 실패는 없었다. 추가 test는 실행하지 않았다.

계측 때문에 수정한 파일은 다음 네 개다.

- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkDsegTrace.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs`
- `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs`

commit은 만들지 않았다.
