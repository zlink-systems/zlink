# Round 41: STREAM tcp 64B 목표 재정렬

- 목표: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`의
  `MULTI_STREAM/tcp/64 = 400,124.6 ops/s` 회복.
- 사용자 정정: 400kops 기준은 `STREAM/tcp/64B`이며, WS/WSS는 이번 판단 기준이 아니다.
- 시작 source 상태: core/perf source diff 없이 clean.

## Clean 재측정

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round41_stream_tcp64_clean_recheck`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_023459_round41_stream_tcp64_clean_recheck.txt`
- 결과: `325,532.2 ops/s`
- baseline 대비: `-18.64%`
- 문제 report `299,395.0 ops/s` 대비: `+8.73%`

## 후보 A: dispatch inflight relaxed counter

- 변경: stream packet/raw dispatch hot path의 `_dispatch_inflight` add/sub를
  `memory_order_acq_rel`에서 `memory_order_relaxed`로 낮췄다.
- 근거: 현재 stream dispatch stop은 inflight wait를 하지 않고, 값은 조회용 counter로만 쓰인다.
- build: `cmake --build core/build -j$(nproc)` 통과.
- test:
  `ctest --test-dir core/build -R 'test_stream_socket|test_socket_with_handler|test_multi_stream_server_reassembly|test_stream_fastpath|test_stream_send_blocking_wakeup|test_stream_threadsafe|test_thread_safe_contract_policy|unittest_msg_view' --output-on-failure`
  통과, 22/22.
- perf report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_023716_round41_stream_inflight_relaxed_candidate.txt`
- 결과: `322,232.2 ops/s`
- 판정: clean보다 낮고 400k와 멀다. 원복.

## 후보 B: single complete packet frame copy fast path

- 변경: packet handler에서 수신 payload가 누적 state 없이 완성된 단일 frame 하나일 때,
  기존 state 재조립 루프를 건너뛰고 header/body 메시지를 바로 만들어 callback했다.
- 주의: 이전 `msg_t::init_view` 기반 zero-copy 후보는 악화됐으므로 view는 쓰지 않았다.
- build: `cmake --build core/build -j$(nproc)` 통과.
- test:
  `ctest --test-dir core/build -R 'test_stream_socket|test_socket_with_handler|test_multi_stream_server_reassembly|test_stream_fastpath|test_stream_send_blocking_wakeup|test_stream_threadsafe|test_thread_safe_contract_policy|unittest_msg_view' --output-on-failure`
  통과, 22/22.
- perf report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_024005_round41_stream_packet_single_frame_copy_candidate.txt`
- 결과: `322,866.8 ops/s`
- 판정: clean보다 낮고 400k와 멀다. 원복.

## 현재 판정

- 이번 라운드의 core-only 후보는 유지하지 않는다.
- `send_mutex` 제거 실험은 이전 로그에서 `375,799.2`에서 `383,141.6 ops/s`까지 회복했지만,
  benchmark-side 변경이므로 이번 core-only 목표의 해법으로 채택하지 않는다.
- `STREAM/tcp/64B`는 현재 clean source에서 약 `325k ops/s`이며, baseline `400k ops/s`와
  아직 약 `18.6%` 차이가 있다.

## baseline 조건 재확인

- baseline report meta:
  - commit: `cb605c6c1`
  - runs: `1`
  - connect_concurrency: `128`
  - load_avg: `0.10 0.36 0.42`
- 현재 clean 단독 재측정 meta:
  - commit: `72d893595`
  - runs: `5`
  - connect_concurrency 기본값: `1024`
  - load_avg: `1.67 2.56 4.43`
- baseline과 같은 connect128 재측정:
  - 명령:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-concurrency 128 --connect-ready-timeout-ms 5000 --results-tag round41_stream_tcp64_clean_connect128_recheck`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_024155_round41_stream_tcp64_clean_connect128_recheck.txt`
  - 결과: `323,392.4 ops/s`
  - 판정: connect concurrency 차이는 400k gap을 설명하지 못한다.

## baseline commit 비교

- `cb605c6c1`의 `core/src/sockets/stream.cpp`와 현재
  `core/src/runtime/sockets/stream/stream.cpp`는 ignore-space 기준으로 include 경로와
  formatting 중심 차이다.
- `cb605c6c1`의 `core/src/sockets/stream_dispatch_send.cpp`와 현재
  `core/src/runtime/sockets/stream/stream_dispatch_send.cpp`도 ignore-space 기준으로
  formatting 중심 차이다.
- 반면 `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`에는 baseline 이후
  `session_t::send_mutex`와 `try_send*`의 `lock_guard`가 추가되어 STREAM echo send가
  benchmark helper에서 직렬화된다.
- 따라서 현재까지의 근거상 400k gap의 주원인은 core stream dispatch 구현 차이가 아니라
  benchmark-side 직렬화일 가능성이 높다. 다만 perf helper 변경은 이번 core-only 범위에서
  채택하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 유지 변경: 없음.
- source candidate는 모두 원복했으므로 보안 하드닝 의미 변경 없음.
