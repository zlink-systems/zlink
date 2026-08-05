# Round 32: STREAM/tcp 64B baseline correction

- 목표: `MULTI_STREAM/tcp/64` 목표 기준을 2026-05-13 baseline으로 바로잡고, perf client/server 변경 없이 core-only 후보를 다시 검토한다.
- baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - `META,commit,cb605c6c1`
  - `connect_concurrency: 128 (default)`
  - `RESULT,current,MULTI_STREAM,tcp,64,throughput,400124.600`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
  - `META,commit,db85a5bef`
  - `connect_concurrency: 128 (default)`
  - `RESULT,current,MULTI_STREAM,tcp,64,throughput,299395.000`
- 정정: WS/WSS STREAM 수치는 별도 기준이다. 이번 target은 `STREAM/tcp/64 = 400kops`다.

## Clean rechecks

1. 이전 clean recheck
   - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round30_stream_tcp_clean_recheck`
   - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_004607_round30_stream_tcp_clean_recheck.txt`
   - result: `323970.600`
2. stats probe
   - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 ZLINK_ASIO_TCP_STATS=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round32_stream_tcp_stats_clean`
   - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_010138_round32_stream_tcp_stats_clean.txt`
   - result: `336983.000`
   - note: runner default `connect_concurrency` was `1024`, so baseline comparison에는 부적합하다.
3. baseline option matched clean recheck
   - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-concurrency 128 --connect-ready-timeout-ms 5000 --results-tag round32_stream_tcp_clean_connect128`
   - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_010152_round32_stream_tcp_clean_connect128.txt`
   - result: `333722.000`
   - delta vs baseline: `-16.59%`
   - conclusion: `connect_concurrency` 차이는 400k gap을 설명하지 못한다.

## Candidate A: dispatch inflight relaxed

- 변경 파일: `core/src/runtime/sockets/stream/stream.cpp`
- 변경 내용: packet/raw dispatch callback 진입/이탈에서 `_dispatch_inflight.fetch_add/sub` 메모리 순서를 `acq_rel`에서 `relaxed`로 낮췄다.
- 근거: `stream_dispatch_stop()`은 callback 내부 중지를 TLS로 막고, `_dispatch_inflight`는 공개 조회용 카운터다. callback payload의 메모리 순서를 이 카운터로 보장할 필요는 없어 보였다.
- build: `cmake --build core/build -j$(nproc)` 통과.
- focused test: `ctest --test-dir core/build --output-on-failure -R 'test_(stream_threadsafe|stream_fastpath|stream_socket|stream_send_blocking_wakeup|multi_stream_server_reassembly|transport_matrix)$'` 통과.
- perf command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-concurrency 128 --connect-ready-timeout-ms 5000 --results-tag round32_stream_tcp_inflight_relaxed`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_010432_round32_stream_tcp_inflight_relaxed.txt`
- result: `335118.600`
- delta vs clean connect128: `+0.42%`
- note: load average was high (`27.28 13.88 10.26`), but even nominal delta is below 5%.
- decision: 효과 없음. 변경은 원복했다.

## Current conclusion

- 최신 clean source는 `STREAM/tcp/64`에서 `333k~337kops` 범위이며, baseline `400124.600`보다 낮다.
- perf helper `send_mutex` 제거 diagnostic은 `375799.200`까지 올렸지만, perf client/server 변경이라 core perf 개선으로 유지할 수 없다.
- core-only STREAM 후보 중 지금까지 실패/폐기한 항목:
  - read drain 상한 조정
  - TCP native send
  - packet frame fast path
  - direct activate read
  - current pipe lookup 중복 제거
  - dispatch inflight relaxed
- 보안 의미를 유지한 근거: WS/WSS pending message copy, mtrie non-recursion, port parsing validation, IPC unlink order, decoder/message/send guard, maxmsgsize policy는 변경하지 않았다.
