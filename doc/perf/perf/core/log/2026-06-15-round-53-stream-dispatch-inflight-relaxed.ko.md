# Round 53: STREAM dispatch inflight counter 완화

- 목표: `MULTI_STREAM/tcp/64B`를 baseline
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`의
  `400,124.6 ops/s`에 가깝게 회복한다.
- 시작 시각: 2026-06-15 KST
- 기준 report:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
  - round52 full:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_042257_round52_full_failure_gate.txt`
  - round52 stream tcp,tls:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_045320_round52_stream_tcp_then_tls_repro.txt`

## 기준 수치

- baseline `MULTI_STREAM/tcp/64B`: `400,124.6 ops/s`.
- problem `MULTI_STREAM/tcp/64B`: `299,395.0 ops/s`.
- round52 full `MULTI_STREAM/tcp/64B`: `330,345.4 ops/s`.
- round52 stream tcp,tls `MULTI_STREAM/tcp/64B`: `323,793.8 ops/s`.

## 가설

1. STREAM packet dispatch는 메시지마다 `_dispatch_inflight`를 증가/감소한다. 이 값은
   카운터 조회와 stop guard에 쓰이며 payload 가시성 보장을 맡지 않는다. `acq_rel`
   대신 `relaxed`를 쓰면 64B echo hot path의 atomic fence 비용을 줄일 수 있다.
2. 병목은 perf stream session의 packet 재조립과 send mutex, 또는 pipe flush 비용이어서
   counter memory order 변경은 측정 잡음보다 작을 수 있다.

선택한 변경: 가설 1. 공개 계약과 benchmark 조건을 바꾸지 않고, dispatch counter의
동기화 강도만 낮춘다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거: WS/WSS pending message copy 제거, mtrie 비재귀화, 포트
  파싱 검증, IPC unlink 순서, decoder/message/send guard, maxmsgsize 정책을
  건드리지 않는다.

## 변경

- 대상 파일: `core/src/runtime/sockets/stream/stream.cpp`
- 변경 내용: stream dispatch callback inflight counter의 `fetch_add/fetch_sub` memory
  order를 `std::memory_order_relaxed`로 낮춘다.

## 검증

- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- test:
  - `ctest --test-dir core/build -R 'test_stream_fastpath|test_stream_threadsafe|test_stream_socket|test_stream_send_blocking_wakeup|test_multi_stream_server_reassembly|test_transport_matrix|test_socket_with_handler' --output-on-failure`
  - 결과: 21/21 통과.
- perf:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round53_stream_inflight_relaxed`
  - runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_045809_round53_stream_inflight_relaxed.txt`
  - result: `MULTI_STREAM/tcp/64B = 328,232.8 ops/s`.
  - completion: success 1, fail 0, status complete.
  - load_avg: `16.32 9.99 6.12`.

## 판정

- 목표 달성 여부: 미달성.
- round49 rebuilt `326,046.4` 대비 `+0.67%`, round52 full `330,345.4` 대비
  `-0.64%`로 noise 범위다.
- baseline `400,124.6` 대비 `-17.97%`다.
- problem `299,395.0` 대비 `+9.63%`지만, 400kops 목표와 반복 +10% 기준을
  만족하지 못한다.
- source 변경은 유지하지 않는다.
