# Core DEALER_DEALER latency 조사 결과

## 결론

원인은 계약 B의 HWM 거절 경로에서 wait token을 연결한 직후 `process_submit_commands()`로 socket mailbox를 동기 처리한 것이다. 100개 client가 비슷한 시점에 HWM에 닿으면 각 `zlink_send_part()` 호출이 앞선 pipe의 credit/activate 명령까지 처리했다. 그 결과 credit 회복이 submit 호출 사이에 직렬로 끼어들고 sender의 재개 시점과 burst 간격이 넓어졌다.

`core/src/runtime/sockets/common/socket_send_complete.cpp:235`에서 이 동기 mailbox 처리를 제거했다. 이미 처리된 wake는 token 연결 뒤 readiness 재검사가 찾고, 아직 queue에 있는 wake는 자기 mailbox notification으로 실행되어 연결된 token을 찾으며, 이후 wake도 같은 token을 찾는다. 따라서 fence와 readiness 재검사는 유지하면서 중복 progress만 없앴다.

## 증거

- 정상 속도 5초 계측에서 0.17.0은 10,271,670개 part 호출 중 1,079회 backpressure가 발생했다. WRITABLE은 정확히 1,079개였고 `completion_recv`는 OK와 NO_DATA를 합쳐 2,158회였다.
- 같은 실행에서 poller wait는 11회뿐이었고 1,079개 socket event를 묶어서 반환했다. pipe마다 별도 syscall wake가 발생하는 wake 폭풍은 아니었다.
- 0.15.1 비교 실행은 8,975,748개 part 호출 중 backpressure가 546회였다. HWM cycle 자체도 현재가 더 잦지만, 수정 뒤에도 5초 동안 backpressure/WRITABLE 1,222회와 poller wait 13회가 유지됐다.
- cycle 수와 HWM 정책을 바꾸지 않은 채 동기 mailbox 처리만 제거하자 1024B p95/p99가 재현 2회 모두 크게 줄었다. 이 전후 차이가 원인을 직접 분리한다.
- Callgrind의 1초 축소 실행은 0.17.0에서 wait 등록 25회, `process_activate_write` 25회, `completion_recv` 50회를 확인했다. 0.15.1은 비슷한 부하에서 `process_activate_write` 23회였다.
- 계약 B 직전 `d713ed19e0`의 당시 runner는 1024B 423kmsg/s, mean 0.109ms였다. 계약 B 구간에서 high-throughput queued 동작과 새 wait 등록 경로가 함께 들어왔다. runner 계약이 달라 절대값 비교보다는 변경 구간 확인에만 사용했다.

## 측정

모든 값은 tcp, 100 clients, 5초, runs=3의 한 invocation 중앙값이다. 처리량 단위는 Kmsg/s, latency 단위는 ms다.

### 1024B

| 상태 | 반복 | 처리량 | mean | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| 0.15.1 | A | 806.588 | 0.871 | 1.861 | 2.978 |
| 0.15.1 | B | 806.857 | 0.853 | 1.930 | 4.221 |
| 0.17.0 수정 전 | A | 886.284 | 1.329 | 4.281 | 10.825 |
| 0.17.0 수정 전 | B | 862.712 | 1.471 | 6.400 | 12.764 |
| 0.17.0 수정 후 | A | 891.062 | 1.220 | 2.456 | 4.081 |
| 0.17.0 수정 후 | B | 854.048 | 1.174 | 2.357 | 5.109 |

수정 후 처리량은 수정 전 범위에 남았다. p95는 수정 전보다 43~63%, p99는 60~62% 줄었다. p99는 0.15.1 범위에 근접했고 p95 격차도 크게 줄었다. mean은 수정 전보다 8~20% 줄었지만 0.15.1보다 35~43% 높다.

### 4096B

| 상태 | 반복 | 처리량 | mean | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| 0.15.1 | A | 373.248 | 595.539 | 877.421 | 976.035 |
| 0.15.1 | B | 353.280 | 627.752 | 917.484 | 1042.331 |
| 0.17.0 수정 전 | A | 385.890 | 625.503 | 1251.922 | 1333.721 |
| 0.17.0 수정 전 | B | 374.848 | 632.296 | 1291.437 | 1385.812 |
| 0.17.0 수정 후 | A | 359.129 | 643.458 | 1298.002 | 1382.032 |
| 0.17.0 수정 후 | B | 333.944 | 713.529 | 1314.853 | 1386.328 |

4096B는 두 버전 모두 수백 ms의 HWM 포화 구간이다. 이번 수정은 이 포화 backlog를 줄이지 않았으며, 수정 후 처리량과 mean은 측정 분산 안에서 오히려 나쁜 쪽이었다. 1024B wake 직렬화 수정의 성공 근거로 4096B 값을 사용하지 않는다.

## 검증

- `test_phase3_completion_contract`, `test_wake_invariants`, `test_stream_send_blocking_wakeup`: 5회 모두 통과
- dev 전체 suite, hotpath gate 제외: 139개 중 138개 통과
- 실패한 `test_backpressure_oneway_matrix_pubsub_regression`: `double free or corruption (!prev)` 1회; 즉시 단독 재실행 통과
- Release+LTO `hotpath_gate`: 4개 cell 모두 통과; 최대 ratio 1.0109
- `git diff --check`: 통과

## BLOCKERS

- 1024B mean은 개선됐지만 0.15.1 수준까지 회복하지 않았다. 계약 B가 payload를 호출자에게 돌려주고 WRITABLE 뒤 다시 제출하게 하는 왕복 자체의 잔여 비용이다. 이를 없애려면 공개 계약을 바꾸거나 Core가 payload를 다시 보유해야 하므로 이번 허용 범위에서는 다루지 않았다.
- 4096B 포화 구간의 p95/p99는 개선되지 않았다. 이를 줄이려면 auto-HWM budget/profile 또는 I/O batching 정책을 별도 성능 과제로 조정해야 한다. 이 변경은 다른 pattern에도 영향을 줄 수 있어 이번 단일 원인 수정에 섞지 않았다.
- dev 전체 suite에서 PUBSUB double-free가 한 번 발생했다. 수정 경로는 SEND WRITABLE을 지원하지 않는 PUBSUB에서 실행되지 않으며 단독 재실행은 통과했으므로 unrelated intermittent failure로 남겼다.

변경 파일은 `core/src/runtime/sockets/common/socket_send_complete.cpp` 하나다. 공개 header, bindings, 보호 문서는 변경하지 않았다.
