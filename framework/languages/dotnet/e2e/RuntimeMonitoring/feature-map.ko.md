# .NET RuntimeMonitoring E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-7-monitoring.ko.md`

## 10.0.0 목표 판정

Config 7은 MeshNode·peer·ChannelName readiness, claim progress와 location health를 공개 runtime
snapshot과 typed event로 검증한다. Publish는 전용 snapshot·metric·runtime event를 만들지 않으므로
Track B에서는 public 관측값의 부재와 result-free terminal을 검증한다. Socket·location·Spot source
marker는 관련 행의 부분 증거로만 기록한다.

| 시나리오 | 상태 | 현재 증거 | 남은 gap |
|---|---|---|---|
| MON-A1 | 구현 | MeshNode의 peer lifecycle event와 후속 snapshot이 같은 RID·generation·endpoint를 나타내는지 확인했다. | 없음 |
| MON-A2 | 구현 | 같은 RID의 정상 교체 전후 typed peer event와 snapshot에서 이전 generation이 ready로 남지 않는지 확인했다. | 없음 |
| MON-A3 | process 통과 | `logs/20260805-154113-2416959/`에서 A→B와 B→A 양방향 request가 각각 remote handler에서 처리되는 것을 확인했다. B의 weight를 `0`으로 바꾸면 A의 snapshot/event가 `ready=False|targets=0`으로 바뀌고, A request가 `NotFound`를 원인으로 HTTP 500 terminal에 도달했으며, weight 복구 뒤 양방향 선택이 다시 가능해졌다. Message trace와 서비스 file log를 함께 보존했다. | 없음 |
| MON-A4A | source 구현·process 미검증 | `MonA4AvailabilityTransitionScenario`가 정상 replacement 뒤 readiness 복원, sequence 증가와 peer event 순서를 aggregate selector로 검사한다. | fresh actual-process runner 실행 log와 role server evidence를 추가해야 한다. |
| MON-A4B | source 구현·process 미검증 | `MonA4AvailabilityTransitionScenario`가 service crash 뒤 stale peer 제거, bounded follow-up과 replacement readiness를 aggregate selector로 검사한다. | fresh actual-process runner 실행 log와 role server evidence를 추가해야 한다. |
| MON-A5 | 구현 | location store 중단·복구 중 peer·channel messaging이 유지되고 location health event와 snapshot이 일치하는지 확인했다. | 없음 |
| MON-A6 | 미구현 | Public placement snapshot의 node·stable type별 사용량, reservation·commit·release와 capacity 거부를 actual process에서 대조하는 selector가 없다. | 역할 server와 runner registration을 추가해야 한다. |
| MON-B1 | 부분 구현 | zero-target publish를 실제로 시작한 뒤 public snapshot·event에 publish 전용 필드가 없고, framework assembly에 제거한 public type·metric·event 이름이 없음을 검사한다. | 막힌 remote target이 있어도 시작 뒤 정상 완료하며 rollback·자동 재시도가 없음을 process E2E에서 추가로 확인한다. |
| MON-B2 | 부분 구현 | local subscriber를 만든 뒤 publish하고 handler 단일 처리와 public snapshot·event의 publish 전용 관측값 부재를 검사한다. | 막힌 local target과 정상 target을 함께 두고 message-flow trace에도 target별 결과가 남지 않음을 추가로 확인한다. |
| MON-C1 | 구현 | application gate 중 별도 request와 정상 observer가 진행되고, 작은 observer queue의 coalescing·consumer 예외 뒤 snapshot resync와 messaging이 유지되는지 확인했다. | 없음 |
| MON-D1A | source 구현·process 미검증 | `MonD1FailureRecoveryScenario`가 등록하지 않은 MeshName과 observer 조회 거부를 aggregate selector로 검사한다. | fresh actual-process runner 실행 log와 role server evidence를 추가해야 한다. |
| MON-D1B | source 구현·process 미검증 | `MonD1FailureRecoveryScenario`가 세 번의 crash/restart 뒤 stale peer 제거, sequence 증가와 messaging recovery를 aggregate selector로 검사한다. | fresh actual-process runner 실행 log와 role server evidence를 추가해야 한다. |

2026-07-20 실행 기록은 당시 계약의 회귀 증거이며 `logs/20260720-004148-1606817`부터
`logs/20260720-004424-1615752`까지 보존한다. CA-D77 이후 제거된 target별 집계를 검증한 기존
MON-B1·MON-B2 결과는 현재 계약의 완료 증거로 사용하지 않는다.
