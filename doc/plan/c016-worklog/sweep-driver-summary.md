# C 1024-byte sweep driver

`sweep.sh` drives one baseline/candidate pair per `(pattern, transport)` cell,
then calls the candidate worktree's regression gate with the corresponding
single or multi report pair. Its result/state files are under this directory.
Runner result roots are explicitly redirected to `runner-results/baseline` and
`runner-results/candidate`, so normal `bindings/c/perf/results` locations are
not used.

## Confirmed interfaces

- Single `--pattern ALL` expands `STANDARD_PATTERNS` to `PAIR`, `PUBSUB`,
  `DEALER_DEALER`, `DEALER_ROUTER`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER`,
  and `ROUTER_ROUTER_REQREP` ([run_benchmarks.sh](/home/hep7/project/zlink/bindings/c/perf/run_benchmarks.sh:184), expansion at [line 575](/home/hep7/project/zlink/bindings/c/perf/run_benchmarks.sh:575)).
- Multi `--pattern ALL` comes from its `PATTERNS` list: `DEALER_DEALER`,
  `DEALER_ROUTER_SENDSEND`, `ROUTER_ROUTER_SENDSEND`,
  `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`, `PUBSUB`, `STREAM`
  ([run_benchmarks_multi.sh](/home/hep7/project/zlink/bindings/c/perf/run_benchmarks_multi.sh:52)).
- Both wrappers accept `--results-tag`; the single wrapper constructs a report
  under `results/single/report` and passes its explicit `--result-file` to the
  Python runner ([run_benchmarks.sh](/home/hep7/project/zlink/bindings/c/perf/run_benchmarks.sh:719), [line 949](/home/hep7/project/zlink/bindings/c/perf/run_benchmarks.sh:949)). The shared runner prints `result_file` in effective options ([run_comparison.py](/home/hep7/project/zlink/bindings/c/perf/run_comparison.py:4118)) and saves the file ([line 4550](/home/hep7/project/zlink/bindings/c/perf/run_comparison.py:4550)).
- Runners print `Perf runtime libzlink: <resolved path>` after resolving the
  worktree local runtime ([run_benchmarks.sh](/home/hep7/project/zlink/bindings/c/perf/run_benchmarks.sh:440)); multi additionally verifies loaded benchmark runtime identity ([run_benchmarks_multi.sh](/home/hep7/project/zlink/bindings/c/perf/run_benchmarks_multi.sh:1522)).
- The gate takes `--baseline-single/--candidate-single` or
  `--baseline-multi/--candidate-multi` ([perf_regression_gate.py](/home/hep7/project/zlink/bindings/c/perf/perf_regression_gate.py:215)); it uses 0.95 for throughput/bandwidth and 1.05 for latency metrics ([line 139](/home/hep7/project/zlink/bindings/c/perf/perf_regression_gate.py:139)). Its unit tests cover the five-percent boundary and report-pair arguments ([test_perf_regression_gate.py](/home/hep7/project/zlink/bindings/c/perf/tests/test_perf_regression_gate.py:47)).

## Design

- It parses each worktree's wrapper declaration at runtime, compares only the
  intersection, and appends absent-side cells as `UNSUPPORTED` with `expected
  unsupported`.
- It checks the candidate `core/build/lib/libzlink.so` (or `bin`) before any
  cell. A newer file in `candidate/core/src` stops the driver immediately.
- Reports are located first from runner stdout's `result_file` and then by
  files created after a per-invocation marker. Each runner call has `timeout
  900`; failures append `ERROR` and the sweep continues. Existing PASS and
  UNSUPPORTED cells resume-skip; failed/error cells need `--retry-failed`.

## Unconfirmed / operational note

The current multi wrapper unconditionally calls `build_core_runtime` before
executing its workload ([run_benchmarks_multi.sh](/home/hep7/project/zlink/bindings/c/perf/run_benchmarks_multi.sh:1449)), even with `--reuse-build`. Therefore the driver itself is only authored and syntax-checked here; it has not been run. Before any real sweep, coordinate with the job owning `core/build` (or revise the runner interface) so this wrapper-side build behavior is acceptable.
