# Round 39: STREAM current-send EAGAIN short circuit

- 시각: 2026-06-15 02:20 KST
- 목표: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`의
  `MULTI_STREAM/tcp/64 = 400124.6` 회복.
- 범위: core source only. perf runner/client/server 코드는 변경하지 않는다.

## Starting point

- Round 38 packet complete-frame view 후보는 `323288.0`으로 악화되어 원복했다.
- 기존 로그상 batch/read-drain/native-send/packet-fastpath/direct-xsend는 400k 복구로
  이어지지 않았다.
- 가장 큰 신호는 perf helper `send_mutex` 제거 진단값이지만, 이는 benchmark-side
  변경이므로 이번 source 후보로 유지할 수 없다.

## Candidate

- 대상: `core/src/api/socket/socket_message_send_api.cpp`
- 가설:
  - STREAM packet callback 안에서 같은 routing id로 `ZLINK_DONTWAIT` 답장을 보낼 때는
    `stream_dispatch_send_current_msg_from_io()`가 먼저 직접 pipe write를 시도한다.
  - 이 direct current-send가 `EAGAIN`이면 일반 routed send로 떨어져 `_api_mutex`,
    routing id parse, route lookup, 일반 send를 한 번 더 수행한다.
  - current context의 direct pipe가 이미 `EAGAIN`이면 같은 DONTWAIT 전송의 일반 routed
    fallback도 성공 가능성이 낮다. 이 중복 fallback을 끊으면 pending이 생기는 구간에서
    server-side `send_mutex` 보유 시간이 줄 수 있다.

## Verification

- Build: `cmake --build core/build` passed.
- Focused CTest:
  `ctest --test-dir core/build -R 'test_stream_socket|test_socket_with_handler|test_multi_stream_server_reassembly|test_stream_fastpath|test_stream_send_blocking_wakeup|test_c_failure_boundary_contract|test_c_behavior_contract|test_c_contract_surface' --output-on-failure`
  passed 11/11. The C binding names in that expression were not present in this CTest set.
- Perf command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round39_stream_current_eagain_short_circuit`
- Runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- Report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_022315_round39_stream_current_eagain_short_circuit.txt`
- Result: `MULTI_STREAM/tcp/64 = 332432.2`.

## Decision

- Revert source candidate.
- Reason: no clear improvement over clean `335068.0`, and still far below corrected baseline
  `400124.6`.
- Interpretation: the routed fallback after current-send `EAGAIN` is not the dominant
  STREAM/tcp/64 cost under this run, or the benefit is below runner noise.
