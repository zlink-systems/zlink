# .NET RuntimeMonitoring E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-7-monitoring.ko.md`

## 10.0.0 목표 판정

Config 7은 MeshNode·peer·ChannelName readiness, claim progress와 location health를 공개 runtime
snapshot과 typed event로 검증한다. Publish는 전용 snapshot·metric·runtime event를 만들지 않으므로
Track B에서는 public 관측값의 부재와 result-free terminal을 검증한다. Socket·location·Spot source
marker는 관련 행의 부분 증거로만 기록한다.

| 시나리오 | 상태 | 현재 증거 | 남은 gap |
|---|---|---|---|
| MON-A1 | 구현 | Host status와 RouteMesh status를 각각 읽고, 초기 no-target Channel 상태를 보관한 뒤 `svc-b` 추가 후 peer·target readiness와 typed event를 actual process에서 확인했다(`logs/20260806-200728-404264`). |
| MON-A2 | 구현 | 같은 RID의 정상 교체 전후 typed peer event와 snapshot에서 이전 generation이 ready로 남지 않는지 확인했다. | 없음 |
| MON-A3 | process 통과 | `logs/20260805-154113-2416959/`에서 A→B와 B→A 양방향 request가 각각 remote handler에서 처리되는 것을 확인했다. B의 weight를 `0`으로 바꾸면 A의 snapshot/event가 `ready=False|targets=0`으로 바뀌고, A request가 `NotFound`를 원인으로 HTTP 500 terminal에 도달했으며, weight 복구 뒤 양방향 선택이 다시 가능해졌다. Message trace와 서비스 file log를 함께 보존했다. | 없음 |
| MON-A4A | 구현 | `MonA4AvailabilityTransitionScenario`가 공통 시나리오의 정상 종료·replacement 순서대로 provider 제거, sequence 증가, 새 RID 하나의 readiness, Channel target 복원과 peer event sequence를 actual process에서 확인했다(`logs/20260806-193035-3612505`). |
| MON-A4B | 구현 | `MonA4AvailabilityTransitionScenario`가 service crash 뒤 old physical RID 제거, replacement physical RID 교체, crash 전후 target 수, channel readiness, 최신 snapshot과 replacement handler의 단일 marker를 actual process에서 확인했다(`logs/20260806-194951-2025087`). |
| MON-A5 | 구현 | Location Store 중단·복구에서 public RouteMesh가 `Degraded`와 `Ready`로 전이하고, 기존 request가 계속 처리되는지 확인했다(`logs/20260806-212420-3865772`). | 없음 |
| MON-A6 | 구현 | 단일 `svc-a`의 actor·spot limit을 각각 1로 설정하고 public create·close API와 `IZLinkRouteMeshRuntime` placement snapshot을 대조했다. 한도 초과는 실패하고 release 뒤 재생성이 성공하며 `IsAvailable`이 복구되는 것을 확인했다(`logs/20260806-194135-923017`). |
| MON-A7 | 미구현 | 없음 — capacity metric reset 시나리오는 아직 .NET e2e fixture에 없다. | `MaxQueuedApplicationJobs = 1`과 handler-start gate로 포화를 만든 뒤 Core HWM·job queue snapshot을 읽고 `ResetCapacityMetrics` 호출 전후로 configuration/current 유지, epoch +1, peak=current, count·duration=0을 actual process에서 확인하는 fixture 구현이 필요하다. |
| MON-B1 | 구현 | 서로 다른 remote process에 blocked·accepted target을 배치하고, blocker handler 진입·`ApplicationReceivePaused`·accepted target의 marker 선처리·gate 해제 뒤 blocked target의 단일 처리와 publish 전후 topology 불변을 검증했다(`logs/20260806-212436-3900074`). | 없음 |
| MON-B2 | 구현 | 같은 process의 두 matching local target을 만들고 한 target의 gate 대기 중 다른 target의 단일 처리를 확인한 뒤 gate 해제 후 대기 target도 한 번 처리하는지 검증했다(`logs/20260806-212453-3908715`). | 없음 |
| MON-C1 | 구현 | application gate 중 별도 request와 정상 observer가 진행되고, 작은 observer queue의 coalescing·consumer 예외 뒤 snapshot resync와 messaging이 유지되는지 확인했다(`logs/20260806-212508-3911604`). | 없음 |
| MON-D1A | 구현 | `MonD1FailureRecoveryScenario`가 등록하지 않은 MeshName과 observer 조회 거부를 aggregate selector로 검사하고 actual process에서 통과했다(`logs/20260806-194450-1143573`). |
| MON-D1B | 구현 | `MonD1FailureRecoveryScenario`가 세 번의 crash/restart 뒤 stale peer 제거, current peer readiness, channel readiness, target 수와 sequence 증가를 actual process에서 확인했다(`logs/20260806-195442-2814675`). 공통 시나리오에 없는 별도 messaging assertion은 제거했다. |

2026-07-20 실행 기록은 당시 계약의 회귀 증거이며 `logs/20260720-004148-1606817`부터
`logs/20260720-004424-1615752`까지 보존한다. CA-D77 이후 제거된 target별 집계를 검증한 기존
기존 target별 집계를 검증한 MON-B1·MON-B2 결과는 현재 완료 증거로 사용하지 않는다. 현재
완료 증거는 위의 공통 시나리오 실행 기록이다.
