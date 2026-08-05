# Round 54: STREAM packet callback lock scope 축소

- 목표: `MULTI_STREAM/tcp/64B`를 baseline
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`의
  `400,124.6 ops/s`에 가깝게 회복한다.
- 시작 시각: 2026-06-15 KST
- 기준 report:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
  - round52 full:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_042257_round52_full_failure_gate.txt`
  - round53:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_045809_round53_stream_inflight_relaxed.txt`

## 기준 수치

- baseline `MULTI_STREAM/tcp/64B`: `400,124.6 ops/s`.
- problem `MULTI_STREAM/tcp/64B`: `299,395.0 ops/s`.
- round52 full `MULTI_STREAM/tcp/64B`: `330,345.4 ops/s`.
- round53 `MULTI_STREAM/tcp/64B`: `328,232.8 ops/s`.

## 가설

1. `stream_dispatch_packet_msg_from_io()`는 per-pipe packet state lock을 잡은 채
   packet handler를 호출한다. handler 안에서 echo send가 실행되므로 lock 보유 시간이
   실제 상태 조립보다 길다. packet state를 local message로 옮긴 뒤 lock 밖에서 handler를
   호출하면 callback 경계의 불필요한 직렬화를 줄일 수 있다.
2. ASIO stream session은 한 connection을 한 I/O thread에서 처리하므로 per-pipe packet
   state 보호 비용은 contention이 아니라 lock/unlock 자체와 handler 중 lock 보유 기간이다.
   실제 병목이 perf helper `send_mutex`이면 개선은 noise 수준일 수 있다.

선택한 변경: packet 완성 시 `state.header/body`를 local `msg_t`로 move하고 `state.reset()`
후 packet lock을 풀고 handler를 호출한다. handler 반환 뒤 다시 lock을 잡아 같은 payload의
남은 packet parsing을 이어간다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거: maxmsgsize 검사는 handler 호출 전에 그대로 수행하고, WS/WSS
  pending message copy 제거, mtrie 비재귀화, 포트 파싱 검증, IPC unlink 순서,
  decoder/message/send guard 정책을 변경하지 않는다.

## 검증

- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- test:
  - `ctest --test-dir core/build -R 'test_stream_fastpath|test_stream_threadsafe|test_stream_socket|test_stream_send_blocking_wakeup|test_multi_stream_server_reassembly|test_transport_matrix|test_socket_with_handler' --output-on-failure`
  - 결과: 21/21 통과.
- perf:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round54_stream_packet_callback_lock_scope`
  - runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_050245_round54_stream_packet_callback_lock_scope.txt`
  - result: `MULTI_STREAM/tcp/64B = 328,632.2 ops/s`.
  - completion: success 1, fail 0, status complete.
  - load_avg: `20.23 18.26 10.85`.

## 판정

- 목표 달성 여부: 미달성.
- round53 `328,232.8` 대비 `+0.12%`, round52 full `330,345.4` 대비 `-0.52%`로
  noise 범위다.
- baseline `400,124.6` 대비 `-17.87%`다.
- source 변경은 유지하지 않는다.
