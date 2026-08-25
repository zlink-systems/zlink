# spec/server 재구성 — 주제 구분 초안

> 47개 문서를 7개 주제로 나눈 초안. 주제 하나를 끝낼 때마다 이 표를 확정 상태로 갱신한다.
> `(A)+(B)`는 구현 스펙 B를 계약 문서 A에 병합한다는 뜻. 여러 계약에 걸치는 구현 스펙은
> 주제 안에서 독립 문서로 남긴다.

| # | 주제 디렉터리 | 포함 문서 | 상태 |
|---|---|---|---|
| 00 | `foundation` | 00 governance, 01 glossary, 02 overview, 03 interaction-model, 04 message-model, 06 framework-api, 32 error-model, 40 layering | 초안 |
| 01 | `execution` | 05 async-execution-policy, 33 core-hwm-job-flow, 46 dispatch-loop, 42 progress-isolation, 43 completion, 41 serialization, 50 payload-ownership, (+ 19 §10·48 말미 shared permit 이관) | 초안 — 경계 사용자 확인 필요 |
| 02 | `channel-transport` | 07 topology, 08 channel-messaging, 09 client-server, 10 listener-identity, 29 transport-liveness (+49 §1만 — §2는 13 mesh-node, §3~§5는 observability로), 51 wire-protocol | 초안 |
| 03 | `spot-actor` | 11 spot-model, 12 spot-messaging, 13 mesh-node, 14 actor-model, 15 spot-actor, 16 address-messaging, 17 stage-wrapper, 18 routing (+45), 47 object-lifecycle | 초안 |
| 04 | `session` | 19 stream-session, 20 session-actor-dispatch (+48) | **파일럿 진행 중** — [매핑표](topics/04-session/mapping.ko.md) |
| 05 | `location-relocation` | 21 location-runtime, 22 location-store, 23 relocation-store, 28 relocation-flow (+44 +52), 30 host-relocation, 31 failover | 초안 |
| 06 | `observability` | 24 monitoring, 25 metrics, 26 flow-tracing, 27 correlation | 초안 |

최상위 `README`는 캠페인 말미에 1장으로 축소한다. 현재 README의 "검증 runner 격리"는 e2e
README로, "디버깅 원칙"·"Trace 비용 규칙"은 observability 주제로 옮긴다.
`languages/`는 그대로 둔다.
