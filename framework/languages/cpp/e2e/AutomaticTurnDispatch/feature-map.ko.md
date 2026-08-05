# C++ AutomaticTurnDispatch E2E feature map

기준 문서는 [Config 8 — 실행 turn](../../../../doc/framework/common/e2e/config-8-execution-turn.ko.md)이다.
아래 표는 정식 시나리오 ID를 한 행씩 기록한다. 기존 runner와 로그는 10.0.0 MeshNode
topology와 `TD-*` 공개 계약을 직접 증명하지 않으므로 완료 근거로 사용하지 않는다.

| 시나리오 | 상태 | 검증 대상 |
|---|---|---|
| `TD-A1` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 세 terminator 공개 표면과 기본 의미. |
| `TD-A2` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 비동기 완료 전 같은 Spot callback 차단. |
| `TD-A3` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 비동기 구간의 Spot 상태 불변식. |
| `TD-A4` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 비동기 대기가 완료 처리를 점유하지 않음. |
| `TD-A5` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 비동기 handler가 같은 Spot timer를 지연. |
| `TD-B1` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: yield 중 같은 Spot의 다른 callback 진행. |
| `TD-B2` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: yield continuation 재삽입 순서. |
| `TD-B3` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: yield 경계의 상태 불변식 비보장. |
| `TD-B4` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: yield 중 같은 Spot timer 진행. |
| `TD-C1` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: HTTP client 작업의 yield 실행. |
| `TD-C2` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: HTTP client 작업의 async 실행과 turn 유지. |
| `TD-C3` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: I/O worker 대기 중 worker thread 비점유. |
| `TD-C4` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: CPU worker 실행과 terminator별 turn 의미. |
| `TD-C5` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: CPU worker에서 blocking I/O 금지. |
| `TD-D1` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 서로 다른 actor의 진행. |
| `TD-D2` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 같은 actor handler 재진입 금지. |
| `TD-D3` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 같은 timer handler 재진입 금지. |
| `TD-E1` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: Entry Spot에서 user Spot으로 join하는 비동기 경계. |
| `TD-E2` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: user Spot 사이 join의 비동기 경계. |
| `TD-E3` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 반대 방향 join의 동시 진행. |
| `TD-F1` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: remote Spot에서도 같은 terminator 의미. |
| `TD-F2` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: MeshNode routed handler의 같은 terminator 의미. |
| `TD-F3` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: session relay actor handler의 같은 terminator 의미. |
| `TD-F4` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: timeout 뒤 turn 해제. |
| `TD-F5` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: cancellation과 shutdown 뒤 turn 정리. |
| `TD-F6` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 상호 대기 cycle의 timeout 종료. |
| `TD-G1` | 전환 필요 | 공개 API와 독립 marker로 검증할 대상: 언어별 terminator 공개 의미 일치. |

## 완료 조건

구현 단계에서 각 행에 대응하는 독립 `TD-* result=passed` marker와 최종
`automatic-turn-dispatch e2e result=passed` marker를 남긴다. runner는 실제 사용한 bindings package
이름, version과 경로를 출력하고, 제거 대상 builder, Spot 전용 router/pub/sub와 별도 중계 계층을
사용하지 않는다는 정적 검사를 통과해야 한다.
