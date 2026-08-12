# Java PUBSUB 후보 검토 결과

## 결론

Java `MULTI_PUBSUB`의 최신 TCP 평균은 `51.44%`로 최소 기준 `70%`에 미달한다. 다음
contract-safe 후보를 C→Java 단독 비교했으나 모두 최신 기준보다 낮아 채택하지 않았다.

| 후보 | C 대비 산술평균 | 결과 |
|---|---:|---|
| topic cache Java byte loop | 42.67% | 원복 |
| single-thread metric collector | 32.98% | 원복 |
| direct outbound native frame | 27.97% | 원복 |
| fixed single-part wrapper re-arm | 27.06% | 원복 |

Sol review는 steady state에서 `TopicMessage`, single parts list, topic 문자열과 receive
wrapper pool이 이미 재사용된다고 확인했다. 남은 비용은 delivery마다 필요한
`subscribe_part`, `msg_size`, `msg_data` Java/native 경계 호출과 Core SUB materialization이다.
이를 줄이려면 Core ABI 확장 또는 `msg_t` 내부 layout 의존이 필요하다. 현재 public contract와
Core release `0.10.1` 제약에서는 두 방법을 채택하지 않는다.

따라서 TCP·WS·WSS·TLS PUBSUB은 같은 receive 경로의 contract-safe 후보가 소진된 상태로
보류한다. 이 판정은 변동성이나 안정성 사유를 사용하지 않는다.
