# Round 199: public send 직렬화 mutex 후보 반려

## 조사 범위

- 시작 runtime: `core/build/lib/libzlink.so.10.6.0`
- 시작 SHA-256: `a57d91a90ae3c0d67ed7d469df848e70038ff700e93fd5989c889795755b3301`
- profile 조건: tcp, 64바이트, 1 peer, active 1초, Spot server Callgrind
- paired 조건: tcp, 64바이트, 100 peer, active 5초, 양쪽 I/O thread 1개
- 비교 기준: peer별 process·context·ROUTER·I/O thread가 같은 paired pattern

Round 198 후보를 원복한 runtime에서 Spot 세 패턴의 server를 다시 profile했다. 계측 오버헤드는
처리량 판정에 사용하지 않고, 호출 경계와 대기 방식만 확인했다.

PUBSUB profile은 전체 1,123,716,491 instruction 중 1,098,877,566 instruction(97.79%)을
`socket_lifecycle_coordinator_t::lock_public_api_sync()` 안에서 사용했다. SENDSEND profile도
전체 5,015,595,809 instruction 중 5,011,165,707 instruction(99.91%)이 같은 함수였다.
Callgrind가 경쟁 중인 thread의 실행 속도를 크게 다르게 만들기 때문에 이 비중을 native CPU
비중으로 해석하지 않는다. 다만 현재 ROUTER public send 직렬화가 lock 소유자가 해제할 때까지
조건 없이 atomic load를 반복하는 busy-spin이라는 코드 사실과, 경쟁 시 대기가 그 함수에
집중된다는 점은 일치한다. REQREP profile에서는 `pthread_mutex_lock` 자체가 전체 instruction의
12.16%였고, node mailbox·completion mutex 비용도 별도임을 확인했다.

profile:

- `/home/hep7/.cache/zlink-core-validation/callgrind.round199.spot-pubsub-server`
- `/home/hep7/.cache/zlink-core-validation/callgrind.round199.spot-reqrep-server`
- `/home/hep7/.cache/zlink-core-validation/callgrind.round199.spot-sendsend-server`

## 대안과 후보

두 대안을 비교했다.

1. 일정 횟수 spin 뒤 `yield`하면 변경 범위는 작지만 scheduler 전환 비용과 기아 가능성을 남기고,
   workload에 맞는 spin 횟수가 새 정책으로 추가된다.
2. 내부 `std::mutex`가 send 직렬화를 소유하면 기존 비재진입 계약을 유지하면서 대기 정책을 운영체제에
   맡긴다. inflight·close·lock 상태를 하나의 atomic bit field에 함께 둔 상태도 분리할 수 있다.

두 번째 대안을 후보로 구현했다. 공개 API와 wire frame, send 순서, close busy 의미는 바꾸지 않았다.
수정한 두 Core 파일은 S3 Channel amendment iteration 3의 70-file snapshot에 포함되지 않았다.

## correctness

별도 Debug build에서 다음 집중 회귀가 모두 통과했다.

- `test_router_concurrent_routed_recv`: 1/1
- `test_router_multiple_dealers`: 1/1
- `test_mesh_node_basic`: 1/1
- `test_mesh_stress`: 1/1
- `test_mesh_peer_admission`: 1/1

후보 공식 runtime SHA-256은
`b6c4fe5aa5bc4ced2c695490b38da1e3d8a49c4a7664c8a9fa795ffe8fe58bf7`였다.

## paired smoke와 판정

결과:
`bindings/c/perf/results/multi/paired/20260720-191700-s9-p02-public-api-sync-mutex-candidate/`

| 패턴 | Spot 처리량 | ROUTER 처리량 | 처리량 비율 | mean 비율 | p95 비율 | p99 비율 |
|------|------------:|----------------:|------------:|----------:|---------:|---------:|
| PUBSUB | 3,426,111.4 msg/s | 3,873,223.4 msg/s | 88.46% | 1.4754 | 2.1270 | 2.6201 |
| REQREP | 68,689.8 ops/s | 101,260.6 ops/s | 67.83% | 3.4741 | 2.3081 | 2.7814 |
| SENDSEND | 70,044.4 ops/s | 120,364.6 ops/s | 58.19% | 1.7001 | 2.3972 | 2.8643 |

Round 197의 5회 중앙값과 비교하면 Spot 절대 처리량은 PUBSUB 약 2.5%, REQREP 약 13.6%,
SENDSEND 약 12.4% 높았다. 처리량 비율과 mean·p95·p99도 세 패턴에서 모두 개선됐다.
그러나 세 처리량 비율은 90% 하한에 미달했고, 모든 패턴에 1.25배를 넘는 지연 항목이 남았다.
PUBSUB 종료 snapshot에는 application message 213개와 13,632바이트가 남았다. REQREP와
SENDSEND의 pending queue, 세 패턴의 multicast drop은 0이었다.

채택 조건은 단발 개선이 아니라 처리량·mean·p95·p99·pending·drop gate를 모두 만족하는 것이다.
따라서 정식 5회 중앙값으로 확장하지 않고 후보의 두 source hunk만 원복했다. 원복 뒤 공식 runtime
SHA-256은 시작 값과 같은 `a57d91a90ae3c0d67ed7d469df848e70038ff700e93fd5989c889795755b3301`이며,
Core source보다 새롭다.

이번 라운드에서는 timeout, assertion, version과 package를 변경하지 않았다. 외부 배포와 bindings
내부 package 배포도 수행하지 않았다. S9-P02와 S9-P03은 계속 진행 중이다.
