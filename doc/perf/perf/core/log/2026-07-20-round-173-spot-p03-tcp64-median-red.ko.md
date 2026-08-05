# Round 173: 안정 runtime의 P03 tcp 64바이트 중앙값 red

## 범위

- runtime: `core/build/lib/libzlink.so.10.6.0`
- runtime SHA-256:
  `671fc61dcf4a462b599e6e2b315b1b1ec9765636351e209df3825fe792b33ffe`
- 조건: tcp, 64바이트, 100 peer, active 5초, cell별 paired 5회
- I/O thread: server 1개, client 1개
- 결과:
  `bindings/c/perf/results/multi/paired/20260720-073137-s9-p03-round173-stable-tcp64-r5/`

앞선 두 성능 후보를 모두 원복하고 correctness gate에서 사용한 안정 runtime으로
P03의 5회 중앙값 조건을 처음 실행했다. 단발 수치가 아니라 같은 cell의 Spot과
ROUTER 결과를 인접하게 교차 실행한 중앙값이다.

## 결과

| 패턴 | Spot 중앙값 | ROUTER 중앙값 | 처리량 비율 | mean 비율 | p95 비율 | p99 비율 |
|------|------------:|----------------:|------------:|----------:|---------:|---------:|
| PUBSUB | 1,869,800.0 msg/s | 4,189,417.0 msg/s | 44.63% | 1.6482 | 1.7108 | 1.7122 |
| REQREP | 70,152.8 ops/s | 109,198.8 ops/s | 64.24% | 3.2349 | 2.1791 | 2.7840 |
| SENDSEND | 75,216.6 ops/s | 133,589.0 ops/s | 56.30% | 1.5219 | 2.0854 | 2.6372 |

세 cell 모두 처리량 90% 하한과 mean·p95·p99 1.25배 상한을 통과하지 못했다.
multicast drop은 0이었고 runner source tree와 runtime SHA는 시작부터 종료까지
변하지 않았다. 따라서 P03은 진행 중인 red 상태이며, 나머지 transport와 payload로
확장하거나 P04 전체 matrix를 시작할 조건은 아직 충족되지 않았다.

## 판정

빈 조회 mutex와 준비 알림 분리는 실제 비용이었지만 세 workload의 중앙값 gap을
설명하는 주 병목은 아니다. 다음 P02 후보는 각 성공 메시지가 공통으로 거치는
mailbox admission, claim 생성·해제, receive view 구성과 wire envelope 처리 비용을
나누어 계측해야 한다. 후보는 이 tcp 64바이트 5회 중앙값에서 세 workload 처리량과
지연이 함께 개선된 뒤에만 유지한다.

이 라운드에서 Core source, version과 package는 변경하지 않았다.
