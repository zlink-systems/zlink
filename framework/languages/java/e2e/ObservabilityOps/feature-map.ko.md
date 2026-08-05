# ObservabilityOps Java feature map

이 표는 공통 Config 11의 시나리오와 Java evidence verifier의 대응을 기록한다. verifier는 배포된
framework 프로세스가 남긴 증거만 읽으며, 런타임 증거를 임의로 만들지 않는다.

| ID | verifier가 확인하는 증거 | 현재 실행 상태 |
|----|--------------------------|----------------|
| OBS-A1 | connector outbound부터 STREAM, relay, Spot dispatch까지 같은 flow와 순서 | 10.0.0 전환 대상 — connector outbound flow가 public 설정으로 기록되지 않음(E2E-JV-32) |
| OBS-A2 | server dispatch error 라인과 원래 request의 같은 flow | PASS |
| OBS-A3 | tracing Off 노드의 기록 억제와 하류의 같은 flow | PASS |
| OBS-A4 | fanout 분기와 timer 발원 flow | PASS |
| OBS-B1 | STREAM active/opened/closed/reconnect와 닫힌 종료 사유 | PASS |
| OBS-B2 | Spot queue와 actor transfer 계기 | PASS |
| OBS-B3 | fanout/lease 계기와 고카디널리티 label 부재 | PASS |
| OBS-B4 | reader 미등록 traffic의 무보관과 messaging 정합 | PASS |
| OBS-C1 | `Relocating → Relocated`, 신규 배치 제외와 기존 연결 유지 | 전환 대상 — 현재 runner는 이전 draining row를 검증함 |
| OBS-C2 | Actor relocation, bound push, pending request와 handoff 계기 | 전환 대상 — 이전 takeover·drain 결과를 현행 relocation 결과로 바꿔야 함 |
| OBS-C3 | User Spot aggregate의 queue·timer·Actor state 복원과 authority commit | 전환 대상 — 현재 runner는 제거 대상인 이전 drain 분기를 실행함 |
| OBS-C4 | `ShutdownAsync`, closing callback, session 종료 통지와 deadline | 전환 대상 — 이전 force-stop·server_drain 결과를 사용함 |
| OBS-C5 | eligible target 부재 시 pre-seal typed blocker | 전환 대상 — 이전 rolling drain zero-target 결과를 사용함 |
| OBS-C6 | 새 version node로 relocation한 뒤 old node Shutdown | 미구현 |
| OBS-C7 | 같은 version planned maintenance relocation 뒤 별도 Shutdown | 미구현 |
| OBS-C8 | Shutdown deadline과 bounded teardown | 미구현 |
| OBS-C9 | Automatic peer Ready 선행 조건과 Manual topology blocker | 미구현 |
| OBS-C10 | relocation mode의 exact application version 선택 | 미구현 |
| OBS-C11 | concurrent Relocate option의 합류와 충돌 | 미구현 |

각 PASS는 현재 계약과 일치하는 실제 Java 역할을 기동한 strict runner와 scenario별 verifier 성공을
뜻한다. 이전 drain 계약으로 통과했던 C track 결과는 현행 Config 11의 PASS가 아니다. OBS-A1은
비공개 환경 변수로 connector trace를 켜지 않고도 같은 flow를 확인할 수 있어야 완료된다. C5의 serving과
zero-target 분기는 같은 프로세스를 임의로 재사용하지 않고 각각 새 토폴로지에서 검증한다.

## 공통 scenario parity gap — 2026-07-29

- `OBS-A5`, `OBS-C12`: 공통 scenario는 추가됐지만 Java actual fixture와 runner selector가 없다.
