# Round 33: SPOT 64B fanout repeat

- 목표: 전체 64B 공통 항목 기준에서 가장 큰 current-vs-problem gap인 `MULTI_SPOT/tcp/64`가 반복 가능한 core hot path 후보인지 다시 확인한다.
- 기준 baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 최신 full current sweep: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_235057_round29_current_64b_sweep.txt`

## Current 64B summary from round29 full sweep

- common 64B count: 26
- current vs problem:
  - 전체 평균: `+0.91%`
  - 전체 중앙값: `+1.12%`
  - one-way 평균: `-5.78%`
  - one-way 중앙값: `-5.31%`
  - echo 평균: `+5.10%`
- worst current vs problem:
  - `MULTI_SPOT/tcp/64`: problem `3896078.6`, current `3276035.2`, delta `-15.91%`
  - `MULTI_SPOT/tls/64`: problem `3739003.6`, current `3389574.4`, delta `-9.35%`
  - `MULTI_PUBSUB/tls/64`: problem `2446707.8`, current `2272469.6`, delta `-7.12%`

## Hypotheses

1. `MULTI_SPOT/tcp/64` gap is repeatable, and remaining cost is in SPOT data-plane publish/local fanout core path.
2. `MULTI_SPOT/tcp/64` gap is run-order or system-load noise; standalone repeat will not show a stable 10% gap.
3. The long-term baseline drop is structural, but current-vs-problem gap is not stable enough for a new source change.

Selected first check: repeat `SPOT` tcp/tls 64B with clean source and unchanged perf runner/client/server. If tcp stays worse than problem by at least 10%, inspect SPOT data-plane fanout path. If not, do not modify source for SPOT this round.

## Initial git state

- core source diff: none at round start.
- untracked previous perf logs exist under `doc/plan/perf/core/log`; leave them untouched.

## Security hardening guard

This round must not weaken:
- WS/WSS pending message full-copy removal
- mtrie non-recursive traversal
- port parsing validation
- IPC unlink order
- decoder/message/send guards
- maxmsgsize policy

## Clean repeat

- command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round33_spot_tcp_tls_repeat`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_010731_round33_spot_tcp_tls_repeat.txt`
- runtime: `core/build/lib/libzlink.so.6.0.4`
- result:
  - `MULTI_SPOT/tcp/64 = 3314983.000`
  - `MULTI_SPOT/tls/64 = 4123758.600`
- 판정: tcp는 problem `3896078.6` 대비 `-14.92%`로 반복 gap이 있다. tls는 problem보다 높아 transport 공통 gap은 아니다.

## Candidate A: skip unchanged local-fanout poller refresh

- 변경 파일: `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
- 변경 내용: `forward_local_fanout()`에서 pending message가 실제로 enqueue된 경우에만 `refresh_poller_interest()`를 호출하도록 했다.
- 근거: 모든 local target 전송이 즉시 성공한 정상 hot path에서는 poller 관심사가 변하지 않는다. pending queue가 생긴 경우에는 기존처럼 poller interest를 갱신한다.
- build: `cmake --build core/build -j$(nproc)` 통과.
- focused test: `ctest --test-dir core/build --output-on-failure -R 'test_(spot_pubsub_scenario|spot_poller|spot_runtime_activation|spot_dispatch_event|spot_router_channel_peer|transport_matrix|multi_socket_contract_regressions)$'` 통과.

Targeted perf:

1. `SPOT tcp,tls` 5-run
   - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round33_spot_local_fanout_refresh_guard`
   - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_011302_round33_spot_local_fanout_refresh_guard.txt`
   - result:
     - `tcp = 3719320.000`
     - `tls = 3336695.800`
   - tcp delta vs clean repeat: `+12.20%`
   - tls delta vs clean repeat: `-19.08%`
   - note: load average was high (`30.32 15.46 11.93`).
2. `SPOT tls` standalone 5-run
   - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round33_spot_tls_refresh_guard_repeat`
   - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_011737_round33_spot_tls_refresh_guard_repeat.txt`
   - result: `4249112.000`
   - 판정: tls regression from the tcp,tls run did not reproduce standalone.
3. `SPOT tcp,tls,ws,wss` 3-run
   - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round33_spot_all_transports_refresh_guard`
   - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_011946_round33_spot_all_transports_refresh_guard.txt`
   - result:
     - `tcp = 3484513.200`
     - `tls = 3546691.800`
     - `ws = 4059849.400`
     - `wss = 3494526.400`
   - vs round30 clean all-transport repeat:
     - tcp `-6.23%`
     - tls `+0.56%`
     - ws `+14.67%`
     - wss `-16.84%`
4. `SPOT wss` standalone 5-run
   - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round33_spot_wss_refresh_guard_repeat`
   - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_012519_round33_spot_wss_refresh_guard_repeat.txt`
   - result: `3552485.800`
   - vs round30 clean all-transport wss `4202064.600`: `-15.47%`

## Decision

- Candidate A is not a clear win.
- It improved `SPOT/tcp` in one 5-run measurement, but the adjacent all-transport check did not repeat a 10% tcp gain and `SPOT/wss` showed a repeated large regression.
- The source change was reverted.
- After revert, `cmake --build core/build -j$(nproc)` passed and core runtime was restored to clean-source behavior.

## Security hardening check

- Candidate A touched only SPOT local fanout poller interest refresh logic.
- It did not touch WS/WSS pending message copy removal, mtrie traversal, port parsing, IPC unlink, decoder/message/send guards, or maxmsgsize policy.
