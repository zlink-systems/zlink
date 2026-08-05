# Round 50: STREAM packet state 직접 callback 후보

- 목표: `MULTI_STREAM/tcp/64B` baseline `400,124.6 ops/s` 회복.
- 시작 시각: 2026-06-15 KST
- 기준 report:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
  - clean: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_040345_round49_stream_current_pipe_resolve_rebuilt_perf.txt`
- 시작 상태:
  - core/perf source diff: empty
  - round47/48/49 logs untracked

## 가설

1. `stream_dispatch_packet_msg_from_io()`는 완성된 `state.header/body`를 로컬
   `msg_t` 두 개로 move하고 state를 reset한 뒤 callback에 넘긴다. packet callback은
   같은 pipe의 packet lock 안에서 동기 실행되므로, state의 header/body를 직접 넘기고
   callback 뒤에 reset하면 move/init 비용을 줄일 수 있다.
2. callback이 message를 닫거나 stream을 조작하는 계약 때문에 state message를 직접
   넘기면 lifecycle 테스트가 실패하거나 개선이 noise 범위에 그칠 수 있다.

선택한 가설: 가설 1. packet callback lifetime은 callback 동안만 유효한 메시지이며,
callback 종료 뒤 `state.reset()`이 닫혔거나 남아 있는 message를 정리한다.

## 읽은 코드

- `core/src/runtime/sockets/stream/stream.cpp`
- `core/src/runtime/core/pipe.cpp`
- `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`
- `core/tests/integration/test_stream_socket.cpp`
- `core/tests/integration/test_stream_threadsafe.cpp`

## 변경

- 임시 변경 파일: `core/src/runtime/sockets/stream/stream.cpp`
- 변경 내용: 완성된 `state.header/body`를 로컬 `msg_t`로 move하지 않고 callback에
  직접 넘긴 뒤 callback 종료 후 `state.reset()`으로 정리했다.
- 최종 변경 상태: core source 변경은 유지하지 않는다. focused test는 통과했지만
  perf 개선이 noise 범위라 원복했다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: STREAM packet dispatch message lifetime.
- 보안 의미를 유지한 근거: maxmsgsize 검증, oversize disconnect, WS/WSS pending
  message, mtrie, 포트 파싱, IPC unlink, decoder/send guard는 변경하지 않는다.
  callback 종료 뒤 state reset으로 header/body storage를 정리한다.
- 추가로 실행한 회귀 테스트: STREAM focused CTest.

## 검증

- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- test:
  - `ctest --test-dir core/build -R 'test_stream_fastpath|test_stream_threadsafe|test_stream_socket|test_stream_send_blocking_wakeup|test_multi_stream_server_reassembly|test_transport_matrix|test_socket_with_handler' --output-on-failure`
  - 결과: 21/21 통과.
- perf:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round50_stream_packet_direct_state_callback`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_040819_round50_stream_packet_direct_state_callback.txt`
  - result: `MULTI_STREAM/tcp/64B = 328,035.6 ops/s`
  - completion: success 1, fail 0, status complete.

## 판정

- 목표 달성 여부: 미달성.
- round49 rebuilt clean `326,046.4` 대비 `+0.61%`, round43 clean
  `320,996.6` 대비 `+2.19%`로 noise 범위다.
- problem `299,395.0` 대비 `+9.56%`지만 400k target과 +10% 반복 개선 기준에는
  미달한다.
- baseline `400,124.6` 대비 `-18.02%`다.
- source 변경은 원복한다.
