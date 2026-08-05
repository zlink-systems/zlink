# Round 49: STREAM current pipe resolve 중복 제거

- 목표: `MULTI_STREAM/tcp/64B`를 baseline
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`의
  `400,124.6 ops/s`에 가깝게 회복한다.
- 시작 시각: 2026-06-15 KST
- 기준 report:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
  - clean sweep: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_025722_round43_current_64b_lowload_sweep.txt`
- 시작 git status:
  - core/perf source diff: empty
  - untracked logs: round47, round48

## 기준 수치

- baseline `MULTI_STREAM/tcp/64B`: `400,124.6 ops/s`
- problem `MULTI_STREAM/tcp/64B`: `299,395.0 ops/s`
- round43 clean `MULTI_STREAM/tcp/64B`: `320,996.6 ops/s`
- perf-helper `send_mutex` 제거 진단은 380k대까지 올랐지만, active plan이
  perf client/server 변경을 금지하므로 유지하지 않는다.

## 가설

1. packet callback echo의 current-send hot path에서 TLS current pipe를 두 번
   조회한다. 이 중복을 없애면 STREAM/tcp/64B에서 작은 개선이 날 수 있다.
2. 주 병목은 perf-helper 직렬화 또는 ASIO TCP 작은 메시지 구조 비용이고, TLS 조회
   중복 제거는 측정 잡음보다 작을 수 있다.

선택한 가설: 가설 1. 변경 범위가 core 내부 helper로 작고, 공개 계약과 perf 조건을
바꾸지 않는다.

## 읽은 코드

- `core/src/runtime/sockets/stream/stream_dispatch_send.cpp`
- `core/src/runtime/sockets/stream/stream_dispatch_send_policy_internal.hpp`
- `core/src/api/socket/socket_message_send_api.cpp`
- `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`

## 변경

- 임시 변경 파일:
  - `core/src/runtime/sockets/stream/stream_dispatch_send_policy_internal.hpp`
  - `core/src/runtime/sockets/stream/stream_dispatch_send.cpp`
- 변경 내용: `stream_dispatch_send_current_msg_from_io()`에서 current pipe를 먼저
  읽은 뒤 helper에 넘겨 TLS current pipe 조회를 한 번 줄였다.
- 최종 변경 상태: core source 변경은 유지하지 않는다. 신뢰 가능한 perf에서 개선폭이
  10% 기준에 못 미쳐 원복했다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: WS/WSS pending message copy 제거, mtrie 비재귀화, 포트
  파싱 검증, IPC unlink 순서, decoder/message/send guard, maxmsgsize 정책을
  건드리지 않는다. STREAM current-send 내부 pipe 조회만 변경한다.
- 추가로 실행한 회귀 테스트: STREAM focused CTest.

## 검증

- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- test:
  - `ctest --test-dir core/build -R 'test_stream_fastpath|test_stream_threadsafe|test_stream_socket|test_stream_send_blocking_wakeup|test_multi_stream_server_reassembly|test_transport_matrix|test_socket_with_handler' --output-on-failure`
  - 결과: 21/21 통과.
- perf:
  - 오염된 측정:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round49_stream_current_pipe_resolve`
    - 결과: `375,705.6 ops/s`
    - 판정: 무효. 이전 perf-helper 임시 변경 후 `bindings/c/build`의 server
      binary가 stale일 수 있는데 `--reuse-build`로 재사용했다.
  - perf binary 재빌드 후 신뢰 가능한 측정:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round49_stream_current_pipe_resolve_rebuilt_perf`
    - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
    - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_040345_round49_stream_current_pipe_resolve_rebuilt_perf.txt`
    - result: `MULTI_STREAM/tcp/64B = 326,046.4 ops/s`
    - completion: success 1, fail 0, status complete.

## 판정

- 목표 달성 여부: 미달성.
- round43 clean `320,996.6` 대비 `+1.57%`로 noise 범위다.
- problem `299,395.0` 대비 `+8.90%`지만 400k target과 +10% 반복 개선 기준에는
  미달한다.
- baseline `400,124.6` 대비 `-18.51%`다.
- source 변경은 원복한다.
