# Round 201: 내부 poller FD 조회 후보 반려

## 조사 근거

Round 200 원복 뒤 공식 runtime은
`core/build/lib/libzlink.so.10.6.0`이고 SHA-256은
`a57d91a90ae3c0d67ed7d469df848e70038ff700e93fd5989c889795755b3301`이었다.
Core source보다 새로운 runtime임을 확인했고, 중지 상태 PID `8508` 외에 실행 중인 성능 측정
process는 없었다.

Round 199의 최신 Spot server Callgrind caller tree를 코드와 다시 대조했다.

- PUBSUB은 전체 instruction의 97.79%가
  `run_ingress_loop -> zlink_poll -> socket_poller_t::rebuild ->
  socket_base_t::getsockopt -> lock_public_api_sync` 경로에 기록됐다.
- SENDSEND도 같은 경로에 99.91%가 기록됐다.
- 두 profile에서 `socket_poller_t::rebuild()` 호출은 각각 6회와 7회였다. Poller가 notification
  FD를 얻기 위해 public `getsockopt(ZLINK_INTERNAL_OPT_FD)`를 호출하고, 동시에 ROUTER send가
  public send 직렬화를 반복해서 획득하면 계측 환경에서 FD 조회가 busy-spin에 머무른다.
- matched trace에서 Spot의 동기화 호출은 ROUTER보다 많았지만, 이 profile 비율은 Callgrind가
  thread 실행 순서를 바꾼 결과도 포함한다. 따라서 native steady-state CPU 비율로 사용하지 않고
  불필요한 public API 경계를 고르는 근거로만 사용했다.

Profile:

- `/home/hep7/.cache/zlink-core-validation/callgrind.round199.spot-pubsub-server`
- `/home/hep7/.cache/zlink-core-validation/callgrind.round199.spot-sendsend-server`
- `/home/hep7/.cache/zlink-core-validation/round174-spot-pubsub-c10.strace`
- `/home/hep7/.cache/zlink-core-validation/round174-spot-reqrep-c10.strace`
- `/home/hep7/.cache/zlink-core-validation/round174-spot-sendsend-c10.strace`
- `/home/hep7/.cache/zlink-core-validation/round170-router-reqrep-c10.strace`

## 대안 비교

두 대안을 비교했다.

1. Poller item을 등록할 때 notification FD를 복사해 저장하면 rebuild에서 socket을 다시 조회하지
   않는다. 하지만 mailbox FD 수명에 관한 지식과 저장 상태가 poller item에도 생긴다.
2. 이미 public admission을 거치지 않는 `get_events_internal()`과 같은 경계로 내부 FD 조회를
   제공한다. Socket이 mailbox 소유권과 FD 조회를 계속 담당하고 poller는 rebuild 시 그 내부 경계를
   사용한다.

두 번째 대안을 후보로 적용했다. 공개 API와 poll 결과, notification FD 수명, socket close 의미는
바꾸지 않았다. `socket_poller.cpp`, `socket_base.hpp`, `socket_base_api.cpp`의 후보 hunk만 수정했다.

## Focused correctness

별도 Debug build에서 다음 6개 suite가 모두 통과했다.

- `test_socket_with_handler`
- `test_multi_socket_contract_regressions`
- `test_mesh_node_basic`
- `test_mesh_stress`
- `test_router_multiple_dealers`
- `test_router_concurrent_routed_recv`

후보 공식 runtime SHA-256은
`26f6fd83d86778ce76cec8aaefc8e2bcfa0e26ad1667a34afc10506a31545d63`이었다.
Runner는 이 runtime 경로를 출력했고 Core source보다 오래된 파일이 없었다.

## Paired gate와 판정

tcp 64바이트, 100 peer, active 5초, server와 client I/O thread 각각 1개 조건으로 Spot 세
패턴과 matched ROUTER를 한 번씩 비교했다.

| 패턴 | Spot 처리량 | ROUTER 처리량 | 처리량 비율 | mean 비율 | p95 비율 | p99 비율 |
|---|---:|---:|---:|---:|---:|---:|
| PUBSUB | 3,603,100.0 msg/s | 4,246,008.0 msg/s | 84.86% | 0.9905 | 2.1264 | 2.6465 |
| REQREP | 65,000.2 ops/s | 113,665.8 ops/s | 57.19% | 3.9806 | 2.9500 | 3.7238 |
| SENDSEND | 69,739.2 ops/s | 138,236.0 ops/s | 50.45% | 2.0646 | 2.8454 | 3.5377 |

PUBSUB mean만 1.25배 상한 안에 있었고 나머지 처리량·지연 조건은 실패했다. PUBSUB peer 종료
snapshot에도 application message 302개와 19,328바이트가 남았다. REQREP와 SENDSEND pending queue,
세 패턴의 multicast drop은 0이었다.

결과:
`bindings/c/perf/results/multi/paired/20260720-194234-s9-p02-internal-poller-fd-candidate/`

후보는 Callgrind 계측 환경의 긴 busy-spin을 제거할 수 있지만 native paired 결과에서 공통 개선과
완전성을 증명하지 못했다. 처리량 90%, mean·p95·p99 1.25배, pending·drop 0을 모두 만족하지 않아
정식 5회로 확장하지 않고 세 파일의 후보 hunk만 원복했다.

원복 뒤 공식 runtime SHA-256은 다시
`a57d91a90ae3c0d67ed7d469df848e70038ff700e93fd5989c889795755b3301`이며 Core source보다 새롭다.
timeout, assertion, version, package와 배포는 변경하지 않았다.
