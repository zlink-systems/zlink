# .NET ResilienceLifecycle E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md`

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| RL-A1 | 구현 | accepted request, SIGTERM drain·row 제거, 같은 endpoint의 새 generation과 follow-up을 확인했다(`logs/20260720-032826-1954338`). |
| RL-A2 | 구현 | SIGKILL 뒤 같은 RID의 다른 endpoint 재승인, 새 generation·`ConnectionReady`, 원래 endpoint 복귀를 확인했다(`logs/20260720-033025-1959507`). |
| RL-A3 | 구현 | reconnect storm 이후 request 흐름 복구를 확인했다(`logs/20260720-033130-1962758`). |
| RL-A4 | 구현 | drain 뒤 green endpoint 전환과 요청 복구를 확인했다(`logs/20260720-033205-1968652`). |
| RL-A5 | 구현 | 반복 provider flap 뒤 정상 요청 복구를 확인했다(`logs/20260720-033235-1970076`). |
| RL-B1 | 구현 | timeout·cancellation 정리와 후속 요청 성공을 확인했다(`logs/20260720-033325-1971852`). |
| RL-B2 | 구현 | in-flight request 중 crash의 종단 결과와 생존 provider·replacement 복구를 확인했다(`logs/20260720-033345-1973712`). |
| RL-B3 | 구현 | graceful shutdown과 topology·traffic 복구를 확인했다(`logs/20260720-033415-1975874`). |
| RL-B4 | 구현 | runtime drain의 신규 요청 차단과 종료 상태를 확인했다(`logs/20260720-033439-1977677`). |
| RL-B5 | 구현 | weight 제외 전 수락된 요청의 완료와 신규 요청 제외를 확인했다(`logs/20260720-033453-1978850`). |
| RL-B6 | 구현 | gray fault 동안 생존 provider 선택과 복구를 확인했다(`logs/20260720-033507-1980090`). |
| RL-C1 | 구현 | 12개 ephemeral client request 정리와 follow-up 요청을 확인했다(`logs/20260720-033520-1980718`). |
| RL-C2 | 구현 | provider crash 뒤 topology 제거·생존 provider 선택·replacement 복구를 확인했다(`logs/20260720-033651-1984623`). |
| RL-C3 | 구현 | 정상 프로세스 종료와 새 owner generation 수렴을 확인했다(`logs/20260720-033732-1985803`). |
| RL-C4 | 구현 | Redis 중단 동안 기존 연결 유지와 store 복구 뒤 정상 수렴을 확인했다(`logs/20260720-033756-1986684`). |
| RL-D1 | 구현 | high-fanout request burst의 전량 완료를 확인했다(`logs/20260720-033826-1987586`). |
| RL-D2 | 구현 | observer fault 격리와 public runtime-error sink의 `zlink.runtime_error`·`observer_failed`·`message_flow_observer` event 단일 관측을 확인했다(`logs/20260720-033838-1988265`). |
| RL-D3 | 구현 | dispatch-error evidence의 `failed`·`no_handler`·`reply_error`·packet 값을 확인했다(`logs/20260720-033851-1989147`). |
| RL-D4 | 구현 | missing handler의 wire Error와 client request 예외, 정상 follow-up Response를 확인했다(`logs/20260720-033905-1990307`). |
| RL-D5 | 구현 | request/send 혼합 burst와 두 evidence 흐름을 확인했다(`logs/20260720-033918-1991019`). |

현행 공통 Config 5에 추가된 아래 시나리오는 `.NET` role server와 runner에 아직 등록되지 않았다.
기존 `RL-A1~D5`가 통과해도 아래 행을 완료로 계산하지 않는다.

| 시나리오 | 상태 | 누락 범위 |
|---|---|---|
| RL-E1 | 미구현 | Orderly disconnect 즉시 반영 |
| RL-E2 | 미구현 | RouteMesh·ClientServer half-open 판정 |
| RL-E3 | 미구현 | Connection lifetime과 stale ACK |
| RL-E4 | 미구현 | Connection loss와 terminal completion |
| RL-E5 | 미구현 | Store 독립과 liveness cleanup |
| RL-F1 | 미구현 | Preflight·admission seal capacity 경쟁 |
| RL-F2 | 미구현 | Actor owner ABA fence |
| RL-F3 | 미구현 | 언어 간 terminal failure 해석 |
| RL-F4 | 미구현 | ClientServer topology·direction command 격리 |
| RL-F5 | 미구현 | Activated seal과 Completed 공개 경계 |
| RL-F6 | 미구현 | Admitted descriptor update fence |
| RL-F7 | 미구현 | Relocated request reply ACK barrier |
| RL-F8 | 미구현 | Manual source의 accepted work와 maintenance capture |
| RL-F9 | 미구현 | Preflight deadline과 seal 경계 |
| RL-F10 | 미구현 | Entry Actor와 SpotWide User Spot 이전 |
| RL-F11 | 미구현 | Readiness-first relocation과 느린 turn 격리 |
| RL-F12 | 미구현 | User Spot queue·Message Follow·timer 자동 복원 |
| RL-F13 | 미구현 | Relocation count·callback·payload permit |
| RL-F14 | 미구현 | Precommit abort의 frozen·hold queue 복원 |
