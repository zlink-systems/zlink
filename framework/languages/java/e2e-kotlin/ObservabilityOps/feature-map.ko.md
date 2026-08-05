# ObservabilityOps Kotlin feature map

이 표는 공통 Config 11의 시나리오와 Kotlin trigger, 공유 Java runtime,
evidence verifier의 대응을 기록한다. verifier는 배포된 framework 프로세스가 남긴
증거만 읽으며, 런타임 증거를 임의로 만들지 않는다.

| ID | verifier가 확인하는 증거 | 현재 실행 상태 |
|----|--------------------------|----------------|
| OBS-A1 | connector outbound부터 STREAM, relay, Spot dispatch까지 같은 flow와 순서 | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-A2 | server dispatch error 라인과 원래 request의 같은 flow | 10.0.0 전환 대상 — Kotlin 역할 host가 아닌 공유 Java runtime의 증거만 검증함(E2E-KT-06) |
| OBS-A3 | tracing Off 노드의 기록 억제와 하류의 같은 flow | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-A4 | fanout 분기와 timer 발원 flow | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-B1 | STREAM active/opened/closed/reconnect와 닫힌 종료 사유 | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-B2 | Spot queue와 actor transfer 계기 | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-B3 | fanout/lease 계기와 고카디널리티 label 부재 | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-B4 | reader 미등록 traffic의 무보관과 messaging 정합 | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-C1 | readiness, typed draining row, 기존 연결, lease 갱신 | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-C2 | takeover, bound push, pending request, handed-off 계기 | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-C3 | 정상 request 뒤 Spot 유지, drain admission seal, accepted turn·actor·STREAM barrier 뒤 local Spot close·row 제거, stale handle의 숨은 원격 생성 금지와 명시적 local `GetOrCreate` 뒤 새 generation | 10.0.0 전환 대상 — Kotlin 역할 host가 없고(E2E-KT-06), 공유 Java runner도 제거 대상인 기존 분기 시나리오를 실행하므로 고정 drain 회귀를 검증하지 않음 |
| OBS-C4 | force stopping, session-closing, server_drain, forced 계기 | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |
| OBS-C5 | serving target 롤아웃과 zero-target deadline 결과 | 10.0.0 전환 대상 — Kotlin 역할 host가 없음(E2E-KT-06) |

현재 runner는 Kotlin 공개 adapter를 사용하는 trigger와 공유 Java runtime 역할을 기동한다. Kotlin
역할 host가 없으므로 scenario별 verifier의 성공은 Kotlin Config 11 완료 증거가 아니다. 각 selector는
새 Redis와 새 토폴로지에서 실행하지만, Kotlin host가 metric·drain·flow를 직접 생성하는 구조로 바꾼 뒤
OBS-A1~C5 전체를 다시 검증해야 한다.

OBS-C2는 동일 routing id를 사용하는 Play 역할의 재기동, pending request 완료와
handed-off 계기를 확인한다. 현재 OBS-C3 selector의 성공은 제거 대상인 기존 분기 시나리오의 결과이며,
공통 Config 11의 고정 drain 회귀 완료 증거로 사용하지 않는다.
