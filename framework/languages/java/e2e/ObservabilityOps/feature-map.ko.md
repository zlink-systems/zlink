# ObservabilityOps Java feature map

이 표는 공통 Config 11의 시나리오와 Java evidence verifier의 대응을 기록한다. verifier는 배포된
framework 프로세스가 남긴 증거만 읽으며, 런타임 증거를 임의로 만들지 않는다.

| ID | verifier가 확인하는 증거 | 현재 실행 상태 |
|----|--------------------------|----------------|
| OBS-A1 | connector outbound부터 STREAM, relay, Spot dispatch까지 같은 flow와 순서 | 10.0.0 전환 대상 — connector outbound flow가 public 설정으로 기록되지 않음(E2E-JV-32) |
| OBS-A2 | server dispatch error 라인과 원래 request의 같은 flow | PASS |
| OBS-A3 | tracing Off 노드의 기록 억제와 하류의 같은 flow | PASS |
| OBS-A4 | fanout 분기와 timer 발원 flow | PASS |
| OBS-A5 | `key_transitions → off → errors_only → key_transitions`에서 정상·오류 요청과 flow trace 억제·재개 | PASS — `logs/20260806-020840-3019399-obs-a5/`, `key=4`, `error=5`, `resumed=9` |
| OBS-B1 | STREAM active/opened/closed/reconnect와 닫힌 종료 사유 | PASS |
| OBS-B2 | Spot queue와 actor transfer 계기 | PASS |
| OBS-B3 | fanout/lease 계기와 고카디널리티 label 부재 | PASS |
| OBS-B4 | reader 미등록 traffic의 무보관과 messaging 정합 | PASS |
| OBS-C1 | `Relocating → Relocated`, 신규 배치 제외와 기존 연결 유지 | 전환 대상 — 현재 runner는 이전 draining row를 검증함 |
| OBS-C2 | Actor relocation, bound push, pending request와 handoff 계기 | 전환 대상 — 이전 takeover·drain 결과를 현행 relocation 결과로 바꿔야 함 |
| OBS-C3 | User Spot aggregate의 queue·timer·Actor state 복원과 authority commit | 전환 대상 — 현재 runner는 제거 대상인 이전 drain 분기를 실행함 |
| OBS-C4 | `ShutdownAsync`, closing callback, session 종료 통지와 deadline | 전환 대상 — 이전 force-stop·server_drain 결과를 사용함 |
| OBS-C5 | eligible target 부재 시 pre-seal typed blocker | 전환 대상 — 이전 rolling drain zero-target 결과를 사용함 |
| OBS-C6 | 새 version node로 relocation한 뒤 old node Shutdown | BLOCKED — `logs/20260806-043125-1370615-obs-c6/relocation.json`이 `RELOCATION_FAILED`를 반환함. public flow에 `one-way submission did not obtain queue capacity before the send deadline`가 기록되어 actor push handoff가 완료되지 않음 |
| OBS-C7 | 같은 version planned maintenance relocation 뒤 별도 Shutdown | BLOCKED — C6와 같은 automatic topology actor push handoff 경로가 해결되지 않아 유효한 C7 public evidence를 만들 수 없음 |
| OBS-C8 | Shutdown deadline과 bounded teardown | PASS — `logs/20260806-043305-1411221-obs-c8/`; public status에서 `FORCE_STOPPED/DEADLINE_EXCEEDED`, callback deadline 일치, forced metric delta 1, terminal 안정성을 verifier가 확인 |
| OBS-C9A | Automatic peer Ready 선행 조건과 target not-ready relocation | BLOCKED — C6와 같은 automatic topology actor relocation/handoff 경로가 해결되지 않아 유효한 C9A evidence를 만들 수 없음 |
| OBS-C10 | relocation mode의 exact application version 선택 | BLOCKED — C6와 같은 automatic topology actor relocation/handoff 경로가 해결되지 않아 exact version 선택 evidence를 검증할 수 없음 |
| OBS-C11 | concurrent Relocate option의 합류와 충돌 | BLOCKED — C6와 같은 automatic topology actor relocation/handoff 경로가 해결되지 않아 concurrent relocation evidence를 검증할 수 없음 |
| OBS-C12 | second waiter cancellation이 shared relocation을 취소하지 않으며 concurrent shutdown 결과가 계약과 일치 | BLOCKED — C6와 같은 automatic topology actor relocation/handoff 경로가 해결되지 않아 concurrent shutdown evidence를 검증할 수 없음 |

각 PASS는 현재 계약과 일치하는 실제 Java 역할을 기동한 strict runner와 scenario별 verifier 성공을
뜻한다. 이전 drain 계약으로 통과했던 C track 결과는 현행 Config 11의 PASS가 아니다. OBS-A1은
비공개 환경 변수로 connector trace를 켜지 않고도 같은 flow를 확인할 수 있어야 완료된다. C5의 serving과
zero-target 분기는 같은 프로세스를 임의로 재사용하지 않고 각각 새 토폴로지에서 검증한다.

## 공통 scenario parity gap — 2026-08-06

- `OBS-A5`는 `A5/` 독립 fixture와 `OBS-A5` selector를 추가해 PASS했다. C6~C12 selector와
  strict verifier는 추가했지만, 실제 public evidence가 완성된 PASS는 `OBS-C8`뿐이다.
- C6, C7, C9A, C10, C11, C12는 automatic topology actor relocation 중 public bound-session
  push handoff가 완료되지 않는 blocker로 남겼다. C9B는 manual topology blocker와 Shutdown은
  확인했지만 Spot evidence가 없어 PASS로 기록하지 않았다.
