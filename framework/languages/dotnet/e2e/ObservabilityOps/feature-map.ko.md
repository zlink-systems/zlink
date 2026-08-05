# ObservabilityOps .NET feature map

이 표는 공통 Config 11의 scenario ID가 `.NET` E2E 앱에서 어떤 증거로 검증되는지 기록한다.
이 앱은 샘플 프로젝트를 참조하지 않으며, session·play·workflow 역할과 검증 client를 자체 계약으로 구성한다.

| ID | 상태 | .NET 구현 |
|----|------|-----------|
| OBS-A1 | 구현 | session request가 actor/API, Play와 room Spot을 같은 flow id로 통과하고 시간 순서가 유지되는지 검증한다. |
| OBS-A2 | 구현 | 미등록 packet의 received/error가 같은 flow id를 사용하고 protocol error가 trigger에 반환되는지 검증한다. |
| OBS-A3 | 구현 | connector가 flow를 만들고 tracing off API node가 기록 없이 flow를 Play로 전달하는지 검증한다. |
| OBS-A4 | 구현 | projection fan-out 두 갈래가 같은 flow id를 사용하고 room timer가 timer origin flow를 만드는지 검증한다. |
| OBS-A5 | source 구현·process 미검증 | 실행 중 diagnostics level을 전환하고 `off`에서 trace 전용 record를 만들지 않으면서 업무 reply가 유지되는지 검증한다. |
| OBS-B1 | 구현 | 세 connector의 active/opened/closed 증감과 reconnect attempt counter를 정확한 meter sample로 검증한다. |
| OBS-B2 | 구현 | Entry/user Spot queue 계열, actor transfer count/duration과 pending request sample을 검증한다. |
| OBS-B3 | 구현 | fanout 1:2, lease renew lateness, 닫힌 label과 고카디널리티 label 부재를 검증한다. |
| OBS-B4 | 구현 | metric reader가 설정되지 않은 node에 raw sample이 보관되지 않는지 검증한다. |
| OBS-C1 | source 구현·process 미검증 | host `Relocate`의 `Relocating`·`Relocated`, 신규 배치 제외와 기존 연결 유지를 검증한다. |
| OBS-C2 | source 구현·process 미검증 | Actor authority와 bound session route가 같은 ObjectGeneration으로 전환되는지 검증한다. |
| OBS-C3 | source 구현·process 미검증 | User Spot과 member Actor aggregate, state reload와 stale global SpotId의 `NotFound`를 검증한다. |
| OBS-C4 | source 구현·process 미검증 | `Shutdown`의 Entry·User·Instance Spot closing과 physical session disconnect 통지를 검증한다. |
| OBS-C5 | source 구현·process 미검증 | PlannedMaintenance와 RollingUpdate의 eligible target이 없을 때 source continuity와 `Blocked/TargetUnavailable`을 검증한다. |
| OBS-C6 | source 구현·process 미검증 | version `N+1` target으로 Actor·User·Instance Spot과 bound session을 이전하고 generation을 유지하는지 검증한다. |
| OBS-C7 | source 구현·process 미검증 | 동일 version target으로 planned maintenance를 완료한 뒤 명시적으로 `Shutdown`하는지 검증한다. |
| OBS-C8 | source 구현·process 미검증 | bounded gate로 closing callback을 막고 deadline, cleanup cancellation과 forced teardown을 검증한다. |
| OBS-C9A | source 구현·process 미검증 | `ObsC9AutomaticConvergenceScenario`가 target readiness 뒤 relocation 결과, workload authority와 후속 traffic을 aggregate selector로 검사한다. fresh actual-process runner 실행 log와 physical readiness evidence를 추가해야 한다. |
| OBS-C9B | source 구현·process 미검증 | `ObsC9ManualTopologyScenario`가 manual topology의 Relocate blocker, source readiness와 workload continuity를 aggregate selector로 검사한다. fresh actual-process runner 실행 log와 physical readiness evidence를 추가해야 한다. |
| OBS-C10 | source 구현·process 미검증 | 같은 topology에서 exact version filter가 placement weight보다 먼저 적용되는지 검증한다. |
| OBS-C11 | 부분 source 구현·process 미검증 | 같은 Relocate intent 합류와 다른 option 차단을 검증한다. Readiness 전용 gate는 아직 source gap이다. |
| OBS-C12 | 부분 source 구현·process 미검증 | 합류 waiter cancellation과 concurrent Shutdown terminal replay를 검증한다. Readiness 전용 gate는 아직 source gap이다. |

## 실행 구조

`Server/Session`, `Server/Play`, `Server/Workflow`는 각 역할을 별도 프로세스로 실행한다.
`Client/Scenarios`와 runner에는 OBS-A1~C12 source가 등록되어 있다. 아직 process 검증 전이므로
완료 증거가 아니다. OBS-C9의 physical readiness gate와 네 manual topology 반복, OBS-C11·C12의
target readiness 전용 gate를 추가하고 전체 runner를 실행해야 Config 11 완료로 판정한다.
