# Config 11 — ObservabilityOps (C++) feature map

정본 시나리오: [config-11-observability-ops.ko.md](../../../../doc/framework/common/e2e/config-11-observability-ops.ko.md)

현재 runner는 `Session`, `Play`, `OrderWorkflow`를 별도 실행 진입점과 역할별 설정 파일로 시작하고,
standalone `Client`도 별도 실행 대상으로 사용한다. 현재 구현된 시나리오의 결과 단언은
`Client/Scenarios/obs_*_scenario.hpp`에 ID별로 분리했고, runner에는 process 수명주기와 drain 중간
상태 확인만 남겼다. 시나리오 결과의 관계 단언은 client가 담당하고, runner는 외부 장애와 역할 수명주기를
결정적으로 만든다.

| 시나리오 | 상태 | 비고 |
|----------|------|------|
| OBS-A1 | `implemented` | connector 발원 flow를 session 수신→route 송신→원격 spot 수신 순서로 대조한다. |
| OBS-A2 | `implemented` | 같은 flow의 수신·dispatch error·`phase=error` 순서를 대조한다. |
| OBS-A3 | `implemented` | tracing-off 노드 전후의 같은 flow를 대조하고 off 노드에는 flow 로그가 없음을 확인한다. |
| OBS-A4 | `implemented` | 한 publish flow가 두 subscriber에 전달되는지 확인하고 timer 발원 flow를 별도로 판별한다. |
| OBS-B1 | `deferred` | server connection 계기는 확인하지만 C++ connector가 정식 `zlink.stream.reconnects` counter를 reader에 노출하고 자동 재접속 시도마다 증가시키는 증거가 없다. |
| OBS-B2 | `implemented` | 다수 room action 뒤 큐 depth·wait를 확인하고 actor 이동 1회와 transfer duration·pending sample 1회를 대조한다. |
| OBS-B3 | `implemented` | fanout 차분 1:2, drop 부재, 금지 label 부재를 확인하고 Redis 외부 지연으로 lease lateness를 만든다. |
| OBS-B4 | `implemented` | metrics-off 노드의 메시징 성공을 확인하고 단위 테스트가 reader 없는 10,000회 계측 뒤 내부 저장 구조 불변을 검증한다. |
| OBS-C1 | `deferred` | 이전 drain 상태 검증을 `Relocating → Relocated`와 신규 배치 제외, 기존 연결 유지 계약으로 전환해야 한다. |
| OBS-C2 | `deferred` | Actor relocation 뒤 bound-session push 연속성, 이동 직전 accepted request와 handoff 계기를 함께 검증해야 한다. |
| OBS-C3 | `deferred` | User Spot aggregate의 accepted queue·timer·Actor state 복원과 authority commit을 검증해야 한다. 현재 runner는 이전 drain 분기를 실행한다. |
| OBS-C4 | `deferred` | `ShutdownAsync`의 closing callback, session 종료 통지와 deadline 결과를 한 실행에서 검증해야 한다. |
| OBS-C5 | `deferred` | `RelocateAsync`가 eligible target 부재 시 admission을 seal하지 않고 typed blocker로 끝나는지 검증해야 한다. |
| OBS-C6 | `deferred` | 새 application version node로만 relocation한 뒤 old node를 Shutdown하는 무중단 patch E2E가 없다. |
| OBS-C7 | `deferred` | 같은 version의 planned maintenance target 선택과 relocation 완료 뒤 별도 Shutdown E2E가 없다. |
| OBS-C8 | `deferred` | Shutdown deadline과 bounded teardown, `OnClosingAsync` 완료·취소 경계를 검증하지 않는다. |
| OBS-C9 | `deferred` | Automatic topology의 peer Ready 선행 조건과 Manual topology relocation blocker E2E가 없다. |
| OBS-C10 | `deferred` | relocation mode별 exact application version target 선택을 검증하지 않는다. |
| OBS-C11 | `deferred` | concurrent `RelocateAsync`의 동일 option 합류와 다른 option 충돌을 검증하지 않는다. |

현재 실행: `./run_e2e.sh [all|flow|metrics|fanout|drain|handoff|force|policy|offnode]`

`drain` selector와 C1~C5 구현은 이전 계약의 흔적이며 현행 Config 11 완료 증거가 아니다.
`deferred` 행은 내부 계기나 테스트 전용 API로 대신하지 않는다. 표에 적은 public 결과와
역할별 evidence가 같은 실행에서 확인된 뒤에만 완료로 바꾼다.
