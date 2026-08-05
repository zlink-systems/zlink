# Round 25: STREAM ws 64B 목표 gap 재정렬

- goal: `STREAM/ws/64B`가 목표 400kops와 round 13의 300kops 근처 기준보다 낮아진 문제를 우선 조사한다.
- 시작 기준 commit: `bc944bded`
- 비교 기준: round 13 `5e3c438a2`, report `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_171942_round13_dealer_stream_64_repeat_current.txt`
- current runtime: `core/build/lib/libzlink.so.6.0.4`
- core 소스 변경: 없음
- perf 소스 변경: 없음

## 문제 재정의

- round 13 `STREAM/ws/64B`: `292,551.0 ops/s`
- round 24 current `STREAM/ws/64B`: `250,055.2 ops/s`
- round 25 5-run repeat `STREAM/ws/64B`: `225,953.4 ops/s`
- 목표 `400,000 ops/s` 대비 current는 약 43% 부족하다.
- `300,000 ops/s` 기준 대비도 current repeat는 약 25% 부족하다.

## source tree 확인

- `git diff 5e3c438a2 bc944bded -- core/src/runtime/transports/ws core/src/runtime/engine/asio core/src/runtime/sockets/stream core/include` 결과: diff 없음
- `git rev-parse 5e3c438a2:core/src/runtime/transports/ws/asio_ws_engine.cpp`와 `HEAD:core/src/runtime/transports/ws/asio_ws_engine.cpp`가 같은 blob이다.
- `git rev-parse 5e3c438a2:core/src/runtime/engine/asio/asio_raw_engine.cpp`와 `HEAD:core/src/runtime/engine/asio/asio_raw_engine.cpp`가 같은 blob이다.

## probes

### target cap 4096

- command:
  - `ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP=4096 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round25_stream_ws_64_targetcap4096_probe`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192628_round25_stream_ws_64_targetcap4096_probe.txt`
- result: success 1, fail 0, `250,884.2 ops/s`
- 판정: target cap 축소는 개선 후보가 아니다.

### transport refresh

- command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round25_stream_64_transport_refresh`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192644_round25_stream_64_transport_refresh.txt`
- result: partial, `STREAM/tcp/64B`만 성공, `tls`에서 fail-fast stop
- `STREAM/tcp/64B`: `309,726.0 ops/s`
- 판정: tcp도 round 13의 `342,798.0 ops/s`보다 낮다. `ws`만의 단독 하락이 아니라 STREAM 계층 또는 시스템 상태 영향이 함께 있다.

### ws 5-run repeat

- command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round25_stream_ws_64_repeat_current5`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192745_round25_stream_ws_64_repeat_current5.txt`
- result: success 1, fail 0, `225,953.4 ops/s`
- 판정: 현재 조건에서 250kops도 안정 상한으로 보기 어렵다.

### resource metrics off

- command:
  - `PERF_DISABLE_RESOURCE_METRICS=1 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round25_stream_ws_64_no_resource_metrics`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192811_round25_stream_ws_64_no_resource_metrics.txt`
- result: success 1, fail 0, `238,898.0 ops/s`
- 판정: resource metrics 오버헤드는 주원인이 아니다.

### 64B gather write

- command:
  - `ZLINK_ASIO_GATHER_WRITE=1 ZLINK_ASIO_GATHER_THRESHOLD=0 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round25_stream_ws_64_ws_gather_probe`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192829_round25_stream_ws_64_ws_gather_probe.txt`
- result: success 1, fail 0, `213,620.8 ops/s`
- 판정: 작은 메시지 gather write는 더 느리다. 후보에서 제외한다.

### stream IO threads 8/8

- command:
  - `PERF_STREAM_SERVER_IO_THREADS=8 PERF_STREAM_CLIENT_IO_THREADS=8 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round25_stream_ws_64_io8_probe`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192852_round25_stream_ws_64_io8_probe.txt`
- result: success 1, fail 0, `231,655.6 ops/s`
- 판정: 단순 IO thread 증설은 개선 후보가 아니다.

## 현재 판정

- `STREAM/ws/64B`는 목표 400kops 대비 명백히 부족하다.
- round 13 이후 core stream/ws source diff가 없으므로, 292kops에서 225-250kops로 내려간 현상은 source regression으로 단정할 수 없다.
- 하지만 400kops 목표는 여전히 미달이며, core hot path 개선 후보가 필요하다.
- 현재까지 버린 후보:
  - target cap 축소
  - resource metrics off
  - 작은 메시지 gather write
  - IO thread 단순 증설

## 다음 후보

- `asio_ws_engine_t::speculative_write()`와 `prepare_output_buffer()`의 64B echo loop에서 반복 encoder 호출, session pull, write_some 재진입 비용을 확인한다.
- tcp도 round 13보다 낮으므로 `asio_engine_t`와 `asio_ws_engine_t`의 공통 STREAM path를 함께 본다.
