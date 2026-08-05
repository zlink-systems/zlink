# Java PERF

Policy-aligned perf suite for `bindings/java`.

This tree is the official Java binding performance surface. It must stay aligned
with:

- `bindings/README.md` perf policy
- `doc/perf/PERF_POLICY.md`
- `doc/perf/PERF_SINGLE_TEST_POLICY.md`
- `doc/perf/PERF_MULTI_TEST_POLICY.md`

The goal is to measure binding-layer cost with comparable messaging scenarios,
not to provide demo code or hide the hot path behind a complex harness.

## Policy Summary

- `single` and `multi` are recv-only.
- `--recv` is not part of the supported wrapper surface.
- Unsupported recv-mode requests fail immediately. Silent skip or fallback is
  not allowed.
- `--runs` is supported on both suites and reports median metrics per
  pattern/transport/size configuration.
- Perf hot paths must keep the messaging loop visible in each pattern file.
- Perf runners must not turn slow fallback paths into the canonical measurement
  path.
- Reports are written under `perf/results/{single,multi}/report/` with
  `Effective Options`, markdown tables, `RESULT,...` lines, and `Completion`
  sections so the executed options are traceable from the saved artifact.

## Entrypoints

- `perf/run_benchmarks.sh` or `perf/single/run_benchmarks.sh`
- `perf/run_benchmarks_multi.sh` or `perf/multi/run_benchmarks.sh`

Each suite builds and runs the Java binding perf entrypoints directly. There is
no shared cross-binding runner, except for the policy-mandated `MULTI_STREAM`
raw client from `bindings/c/perf/common/streamclient`.

## Layout

- `perf/common/` shared Java perf utility
- `perf/single/Zlink.BindingBench/` single-suite sources
- `perf/multi/Zlink.BindingBench.Multi/` multi-suite sources
- `perf/results/single/report/`
- `perf/results/multi/report/`

The runnable Gradle subprojects are:

- `:perf-single`
- `:perf-multi`

## Patterns

Single suite pattern files:

- `PAIR`
- `PUBSUB`
- `DEALER_DEALER`
- `DEALER_ROUTER`
- `ROUTER_ROUTER`
- `SPOT`

Multi suite pattern files:

- `MULTI_DEALER_DEALER`
- `MULTI_DEALER_ROUTER`
- `MULTI_ROUTER_ROUTER`
- `MULTI_PUBSUB`
- `MULTI_SPOT`
- `MULTI_SPOT_REQREP`
- `MULTI_SPOT_SENDSEND`
- `MULTI_STREAM`

Each messaging pattern stays in its own source file so the hot path remains
readable and reviewable.

## Verification

Normal verification should use the wrapper scripts because they enforce the
policy-supported modes and save the measured output in the documented format.

- Single: recv-only receive path
- Multi: recv-only receive path
- Multi STREAM uses the shared core stream client path required by policy.
- Both suites print the same report body to stdout and to the saved report file.
- Multi uses server/client context I/O threads `4` by default, matching the C
  multi policy. Override only with the documented `--io-threads`,
  `--server-io-threads`, or `--client-io-threads` options.
- Benchmark contexts set each size case as `autoHwmMessageUnitBytes` and then
  call context auto-HWM recalculation before active measurement. The value is
  the HWM planning unit, not a payload size limit.

## Smoke

```bash
./perf/run_benchmarks.sh --pattern ALL --msg-sizes 64 --duration 1
./perf/run_benchmarks_multi.sh --pattern ALL --msg-sizes 64 --clients 4 --duration 1
```
