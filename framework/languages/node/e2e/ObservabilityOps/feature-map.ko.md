# Node.js ObservabilityOps 검증표

이 fixture는 Config 11 전용 `Session` 1개, `Play` 2개, `OrderWorkflow` 2개와 브라우저 trigger
client를 실행한다. 모든 역할은 한 Redis location store를 공유하며, 다른 E2E runner나 in-process
contract test의 결과를 대신 사용하지 않는다.

| 시나리오 | 실제 검증 내용 | 통과 로그 |
|----------|----------------|-----------|
| OBS-A1 | STREAM 요청의 flow가 Session과 Play actor 경계를 같은 UUIDv7로 통과 | `log/20260715-145426-3875923` |
| OBS-A2 | 알 수 없는 packet의 dispatch error에도 flow 기록 | `log/20260715-145500-3877445` |
| OBS-A3 | flow 기록을 끈 Session을 지나도 하류 Play에 flow 전파 | `log/20260715-145505-3878067` |
| OBS-A4 | 두 Workflow subscriber의 같은 fanout flow와 timer 발원 flow | `log/20260715-145540-3879939` |
| OBS-B1 | 실제 STREAM 세션 3개의 active/opened/closed와 connector reconnect 계기 | `log/20260715-145545-3880678` |
| OBS-B2 | 실제 actor/spot queue와 transfer 계기 | `log/20260715-145551-3881433` |
| OBS-B3 | fanout 1:N 계기와 Redis 지연으로 만든 owner lease lateness | `log/20260715-145625-3883168` |
| OBS-B4 | meter provider를 끈 Play의 정상 messaging과 빈 metric snapshot | `log/20260715-145633-3883819` |
| OBS-C1 | 전환 대상 — `Relocating → Relocated`, 신규 배치 제외와 기존 연결 유지 | 없음 — 기존 draining 로그는 현행 완료 증거가 아님 |
| OBS-C2 | 전환 대상 — Actor relocation, bound push, pending request와 handoff 계기 | 없음 — 기존 drain 기반 handoff 로그는 현행 완료 증거가 아님 |
| OBS-C3 | 10.0.0 전환 대상 — 정상 request 뒤 Spot 유지, drain admission seal, accepted turn·actor·STREAM barrier 뒤 local Spot close·row 제거, stale handle의 숨은 원격 생성 금지와 명시적 local `GetOrCreate` 뒤 새 generation을 검증해야 한다. 현재 fixture는 제거 대상인 기존 분기 시나리오를 실행한다. | 없음 — 기존 로그는 고정 drain 회귀의 완료 증거가 아님 |
| OBS-C4 | 전환 대상 — `ShutdownAsync`, closing callback, session 종료 통지와 deadline | 없음 — 기존 ServerDrain 로그는 현행 완료 증거가 아님 |
| OBS-C5 | 전환 대상 — eligible target 부재 시 pre-seal typed blocker | 없음 — 기존 zero-target drain 로그는 현행 완료 증거가 아님 |
| OBS-C6 | 새 version node로 relocation한 뒤 old node Shutdown | 미구현 |
| OBS-C7 | 같은 version planned maintenance relocation 뒤 별도 Shutdown | 미구현 |
| OBS-C8 | Shutdown deadline과 bounded teardown | 미구현 |
| OBS-C9 | Automatic peer Ready 선행 조건과 Manual topology blocker | 미구현 |
| OBS-C10 | relocation mode의 exact application version 선택 | 미구현 |
| OBS-C11 | concurrent Relocate option의 합류와 충돌 | 미구현 |

현재 `run_e2e.sh all`은 A1~C5만 등록한다. C1~C5를 현행 계약으로 전환하고 C6~C11을 추가하기 전에는
`observability-ops e2e result=passed`가 Config 11 전체 완료를 뜻하지 않는다.
