# .NET ExecutionTurn E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-8-execution-turn.ko.md`

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| TD-A1 | 구현 | request, actor join, worker, framework HTTP client가 `Submit`/`Async`/`Yield`를 공개하고 blocking 완료 API를 노출하지 않는지 확인한다. |
| TD-A2 | 구현 | `Async` 대기 뒤 continuation과 completion이 끝난 다음 같은 Spot probe가 실행되는 순서를 확인한다. |
| TD-A3 | 구현 | 같은 Spot에서 여덟 개 read-modify-write를 `Async`로 실행하고 counter가 정확히 8인지 확인한다. |
| TD-A4 | 구현 | 1초 `Async` 대기의 응답이 별도 completion 경로로 도착해 timeout 없이 재개되는지 확인한다. |
| TD-A5 | 구현 | `Async` 대기 중 같은 Spot timer가 지연되고 대기 완료 뒤 실행되는지 확인한다. |
| TD-B1 | 구현 | `Yield` 대기 중 같은 Spot probe가 실행되고 continuation이 이후 재개되는지 확인한다. |
| TD-B2 | 구현 | `Yield` continuation 앞에 큐에 들어간 세 probe가 순서대로 실행되는지 확인한다. |
| TD-B3 | 구현 | 여덟 개 read-modify-write가 `Yield` 구간에서 같은 이전 값을 관측해 lost update가 발생함을 확인한다. |
| TD-B4 | 구현 | `Yield` 대기 중 같은 Spot timer가 실행되는지 확인한다. |
| TD-C1 | 구현 | DI로 주입한 framework HTTP client의 `Yield`가 외부 HTTP API 대기 중 Spot probe를 허용하는지 확인한다. |
| TD-C2 | 구현 | 같은 HTTP 호출의 `Async`가 completion까지 Spot turn을 유지하는지 확인한다. |
| TD-C3 | 구현 | CPU worker pool보다 많은 비동기 HTTP 작업을 `RunIoWorker(...).Yield(...)`로 완료하고 `WorkerQueueFull`이 없는지 확인한다. |
| TD-C4 | 구현 | CPU worker 스레드 증거와 `Async`/`Yield`에 따른 같은 Spot probe 순서 차이를 확인한다. |
| TD-C5 | 구현 | CPU worker delegate에 blocking I/O 언래핑이 없는지 source gate로 확인한다. |
| TD-D1 | 구현 | actor A가 `Yield` 중일 때 actor B handler가 실행되는지 확인한다. Session route commit ACK가 검증한 owner RID를 Steady authority 정리 뒤까지 보존해 ingress를 unseal한다. 실제 실행: `logs/20260728-050239-3599515`. |
| TD-D2 | 구현 | actor A의 `Yield` 구간에도 같은 actor A의 두 번째 handler가 재진입하지 않는지 확인한다. 실제 실행: `logs/20260728-050340-3689192`. |
| TD-D3 | 구현 | timer의 `Yield` 구간에도 같은 timer의 다음 tick이 이전 tick 완료 뒤 시작하는지 확인한다. |
| TD-E1 | 구현 | Entry Spot actor handler의 `JoinSpot(...).Async(...)`가 user Spot join을 완료하는지 확인한다. |
| TD-E2 | 구현 | user Spot actor handler가 `JoinSpot(...).Defer()`로 이동을 등록하고 `OnJoinCompletedAsync(...)`에서 다른 user Spot으로 이동 완료를 받는지 확인한다. |
| TD-E3 | 구현 | 서로 반대 방향으로 시작한 두 user Spot join이 모두 timeout 없이 완료되는지 확인한다. |
| TD-F1 | 구현 | 다른 노드의 Spot request를 기다린 continuation이 caller 노드로 돌아오는지 확인한다. |
| TD-F2 | 구현 | MeshNode routed path로 도달한 `play-b` Spot에서도 `Yield` 의미와 marker 순서가 같은지 확인한다. |
| TD-F3 | 구현 | session relay로 도달한 actor handler에서도 `Yield`의 mailbox 의미가 같은지 확인한다. 실제 실행: `logs/20260728-050340-3689192`. |
| TD-F4 | 구현 | request timeout 뒤 같은 Spot probe가 정상 실행되는지 확인한다. |
| TD-F5 | 구현 | cancellation 뒤 같은 Spot probe가 정상 실행되며 별도 shutdown runner가 runtime 종료와 recovery를 확인한다. |
| TD-F6 | 구현 | 현재 Spot으로 되돌아오는 `Async` request가 같은 claim 검증에서 `InvalidOperation`으로 거부되고 다음 probe가 실행되는지 확인한다. |
| TD-G1 | 구현 | 공통 terminator 표면과 `Async`/`Yield` marker 순서를 .NET 결과로 고정한다. |

## Selector 실행 증거

Runner는 `TD-A1`부터 `TD-G1`까지의 canonical ID만 받는다. 알 수 없는 ID는
build와 fixture 시작 전에 종료한다. Client는 실제 실행한 scenario 수를 출력하고,
runner는 선택한 수와 scenario별 완료 marker 수가 모두 같은지 확인한다.

2026-07-28 `TD-A2` 단독 실행은 scenario 1개를 실제 실행해 통과했다.
`TD-Z9`는 fixture를 시작하지 않고 exit code 64로 실패했다. 따라서 scenario 0개를
실행하고 source gate만 통과한 이전 실행은 E2E 완료 증거로 사용하지 않는다.

현행 공통 Config 8의 아래 시나리오는 아직 selector에 등록되지 않았다.

| 시나리오 | 상태 | 누락 범위 |
|---|---|---|
| TD-D4 | process 통과 | `logs/20260805-103107-1129457/`에서 Actor A의 `Async` 대기 중 Actor B handler가 먼저 완료되고, 같은 Actor A의 후속 request는 첫 handler 뒤 FIFO로 완료되는 것을 file evidence와 message-flow trace로 확인했다. |
| TD-D5 | process 통과 | 같은 로그에서 지원하지 않는 context의 `Yield`가 `InvalidOperation`으로 제출 전에 끝나고 remote handler evidence가 없으며, `Async` 대조 경로는 정상 완료되는 것을 확인했다. |
| TD-D6 | process 통과 | 같은 로그에서 Actor self-request와 Spot same-gate `Async`가 `InvalidOperation`으로 거부되고, one-way send와 후속 probe는 정상 동작하는 것을 확인했다. |
| TD-E2A | process 통과 | 같은 로그에서 exception·cancellation handler failure 뒤 deferred Join callback과 target/source lifecycle marker가 생성되지 않고 source Actor가 계속 요청을 처리하는 것을 확인했다. |
| TD-F5A | process 통과 | `logs/20260805-103328-1139599/`에서 pending `Async`가 `Shutdown` 뒤 public stream error로 terminal 처리되고, 재시작한 Play가 같은 Spot에 recovery probe를 한 번 처리하는 것을 file evidence와 message-flow trace로 확인했다. |
