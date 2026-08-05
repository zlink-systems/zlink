# Round 192: Spot outbound admission gate 후보 반려

## 후보

모든 application wire 전송은 `wire_send_mutex`와 MeshNode mutex를 함께 획득한 뒤
`DRAINING` 여부를 확인한다. multicast는 target마다 이 절차를 반복하므로 세 Spot
패턴에 공통으로 존재하는 outbound 비용인지 검증했다.

두 대안을 비교했다.

1. MeshNode의 public lifecycle state 전체를 atomic으로 바꾸면 여러 API의 상태 접근
   규칙까지 함께 바뀌어 변경 범위가 커진다.
2. 새 application 전송 admission만 나타내는 내부 atomic gate를 두면 public state는
   계속 MeshNode mutex가 소유하고 wire hot path의 상태 확인만 분리할 수 있다.

두 번째 대안을 구현했다. start가 `STARTED`를 커밋할 때 gate를 열고 shutdown 또는
강제 destroy가 `DRAINING`/`STOPPED`를 커밋할 때 transport teardown보다 먼저 닫았다.
이미 승인된 blocking send의 취소와 이후 submit의 `ESHUTDOWN` 의미는 유지했다.

## correctness와 paired 결과

별도 Debug build에서 blocking wire send와 shutdown 경합 1개, MeshNode basic 14개,
lifecycle contract 14개, monitor matrix 8개가 모두 통과했다. 이어 공식
`core/build`를 candidate SHA-256
`faead9d834d71f62b0ba435ad67b49013c7df12f9c5ce2a83012e954876fa191`로 만들고
tcp·64바이트·100 peer·5초 paired 1회를 실행했다.

결과:
`bindings/c/perf/results/multi/paired/20260720-091052-s9-p02-round192-send-admission-gate-c100/`

| 패턴 | Spot 처리량 | ROUTER 처리량 | 비율 | mean 비율 | p95 비율 | p99 비율 |
|------|------------:|----------------:|-----:|----------:|---------:|---------:|
| PUBSUB | 2,015,730.6 msg/s | 4,146,954.2 msg/s | 48.61% | 2.0585 | 1.4508 | 1.4944 |
| REQREP | 61,851.6 ops/s | 80,341.2 ops/s | 76.99% | 2.3023 | 1.8873 | 2.3333 |
| SENDSEND | 69,092.2 ops/s | 115,092.8 ops/s | 60.03% | 0.6538 | 2.1802 | 2.8490 |

세 처리량 비율이 모두 90%에 미달했고 지연 gate도 통과하지 못했다. 안정 5회
중앙값과 비교하면 Spot 절대 처리량은 PUBSUB만 7.8% 높고 REQREP와 SENDSEND는
각각 11.8%, 8.1% 낮았다. 단발 paired 비율 상승은 같은 실행의 ROUTER 변동을
분리하지 못하므로 세 workload의 공통 개선을 입증하지 않는다. 따라서 5회 paired
median으로 확장하지 않았다.

## 판정과 원복

후보 전체를 원복했다. 공식 runtime을 다시 빌드한 뒤 SHA-256이 안정 값
`671fc61dcf4a462b599e6e2b315b1b1ec9765636351e209df3825fe792b33ffe`로 복원됐고,
`core/src`·`core/include`보다 오래된 runtime이 없다. version과 package는 변경하지
않았다.

다음 후보는 mutex 횟수의 추가 축소보다 `sched_yield`가 발생하는 server 재시도와
reply/send completion 경로의 실제 CPU 비용을 먼저 분리해 선택한다.
