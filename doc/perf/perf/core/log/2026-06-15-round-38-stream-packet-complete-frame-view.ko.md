# Round 38: STREAM packet complete-frame view

- 시각: 2026-06-15 02:14 KST
- 목표: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`의
  `MULTI_STREAM/tcp/64 = 400124.6` 기준을 회복한다. WS/WSS는 이번 판단 기준에서
  분리한다.
- 범위: core source only. perf runner/client/server 코드는 변경하지 않는다.

## Starting point

- `git diff -- core/src core/include core/tests bindings/c/perf`: empty.
- 최근 clean recheck: `MULTI_STREAM/tcp/64 = 335068.0`.
- baseline commit replay: `MULTI_STREAM/tcp/64 = 381021.6`.
- perf helper에서 `send_mutex`를 제거한 진단값은 약 `383k`였지만, 이는 benchmark-side
  변경이므로 유지하지 않는다.

## Candidate

- 대상: `core/src/runtime/sockets/stream/stream.cpp`
- 이유: STREAM perf server는 `zlink_stream_packet_handler`를 사용한다. 현재 packet
  dispatch는 프레임이 하나의 `msg_t`에 이미 완전히 들어온 경우에도 `state.header`와
  `state.body`에 복사한 뒤 `move`해서 콜백에 넘긴다.
- 변경 방향: `state`가 깨끗한 prefix 단계이고 현재 payload 안에 완전한 packet frame이
  있을 때는 `msg_t::init_view()`로 header/body 구간을 만들어 콜백에 넘긴다. 분할
  프레임과 maxmsgsize 오류 처리는 기존 경로를 유지한다.

## Verification

- Build: `cmake --build core/build` passed.
- Focused CTest:
  `ctest --test-dir core/build -R 'test_stream_socket|test_socket_with_handler|test_multi_stream_server_reassembly|test_stream_fastpath|unittest_msg_view' --output-on-failure`
  passed 11/11.
- Perf command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round38_stream_packet_complete_frame_view`
- Runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- Report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_021658_round38_stream_packet_complete_frame_view.txt`
- Result: `MULTI_STREAM/tcp/64 = 323288.0`.

## Decision

- Revert source candidate.
- Reason: worse than clean recheck `335068.0` and still far below corrected baseline
  `400124.6`.
- Interpretation: `msg_t::init_view()` did not reduce the hot-path cost here. The slice
  bookkeeping and callback lifetime handling cost more than the avoided body copy for 64B.
