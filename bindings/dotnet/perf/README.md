# Dotnet PERF

`bindings/dotnet/perf` is the official performance surface for the .NET
binding. It exists to measure binding-layer cost, expose regressions, and keep
scenario shape aligned with `core/perf` without hiding hot-path behavior behind
extra wrappers.

This suite follows the binding-wide perf rules in
[`bindings/README.md`](../../doc/spec/bindings/README.md) and must also be reviewed against:

- [`doc/perf/PERF_POLICY.md`](../../doc/perf/PERF_POLICY.md)
- [`doc/perf/PERF_SINGLE_TEST_POLICY.md`](../../doc/perf/PERF_SINGLE_TEST_POLICY.md)
- [`doc/perf/PERF_MULTI_TEST_POLICY.md`](../../doc/perf/PERF_MULTI_TEST_POLICY.md)

## Entrypoints

- `./perf/run_benchmarks.sh`
- `./perf/run_benchmarks.ps1`
- `./perf/run_benchmarks_multi.sh`
- `./perf/run_benchmarks_multi.ps1`
- `./perf/single/run_benchmarks.sh`
- `./perf/multi/run_benchmarks.sh`
- `./perf/run_comparison.py`

Top-level shell scripts are the stable user entrypoints. They dispatch to the
local single or multi runner in this directory. The compatibility adapter
`run_comparison.py` accepts the older core-style invocation and forwards to the
same local runners.

## Current Scope

The current suite exposes the entrypoints and patterns wired in this
repository. Single and multi shell entrypoints follow the perf policy surface
from `bindings/README.md` for CLI names, defaults, result file naming, and the
`## Effective Options (start)` report header.

- single:
  - patterns: `PAIR`, `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`, `ROUTER_ROUTER`
  - public receive surface: `recv`
  - default transports: `tcp,tls,ws,wss,inproc,ipc` on Linux and `tcp,tls,ws,wss,inproc` on Windows
  - default sizes: `64,256,1024,65536,131072,262144`
- multi:
  - entrypoint: `./perf/run_benchmarks_multi.sh`
  - default patterns: `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER`, `MULTI_ROUTER_ROUTER`, `MULTI_PUBSUB`, `MULTI_STREAM`
  - public receive surface: `recv`
  - default transports: `tcp,tls,ws,wss`
  - default clients: `100` and `10000` for `MULTI_STREAM`
  - default sizes: `64,256,1024,65536,131072,262144`
  - stream default sizes: `64,256,1024,65536`
  - runner behavior: the client is pointed at the server's emitted `READY,<endpoint>` value so the measured path matches the actual bind endpoint
  - `MULTI_STREAM` keeps a single canonical pattern surface in the runner and report output

## Design Constraints

- Perf code is not sample code. Convenience is secondary to measurement fidelity.
- Each messaging pattern keeps its own source file under `single/.../src` or `multi/.../src`.
- Hot-path send/recv/publish/subscribe logic stays visible in the pattern file instead of being hidden behind broad helper layers.
- Results use the official `RESULT,current,...` metric format so they can be
  compared with the core perf shape.
- Multi reports normalize pattern names to `MULTI_*` in the saved report even
  when the underlying process emits the base pattern token.
- Multi STREAM keeps a single canonical `MULTI_STREAM` public surface.
- Multi STREAM uses the shared core stream client path required by policy.

## Execution

Examples:

```bash
./perf/run_benchmarks.sh --pattern PAIR --transports inproc --msg-sizes 64 --duration 1
./perf/run_benchmarks_multi.sh --pattern MULTI_DEALER_DEALER --transports tcp --msg-sizes 64 --clients 4 --duration 1
python3 ./perf/run_comparison.py PAIR --duration 1
```

The shell runners execute the existing Release benchmark outputs produced by the
Gate 1 `dotnet build` step.
Multi runs spawn separate server/client processes and write raw logs under
`results/multi/tmp/` before extracting report metrics.
Managed runs set `DOTNET_TieredCompilation=1`,
`DOTNET_TC_QuickJitForLoops=1`, and `DOTNET_ReadyToRun=1` so the measured
active window uses the same tiered-runtime policy as `doc/perf/PERF_POLICY.md`.

Multi uses `PERF_IO_THREADS=4` by default for both server and client processes,
matching the C multi policy. Benchmark processes set the current message size
as `Context.Options.AutoHwmMessageUnitBytes` and then call context auto-HWM
recalculation before active measurement.

## Results

Results are stored under:

- `results/single/report`
- `results/multi/report`
- `results/multi/tmp`

Report filenames follow:

- `perf_dotnet_<suite>_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt`

Saved reports start with:

- `## Effective Options (start)`

and include the required metrics:

- `throughput`
- `bandwidth`
- `latency`
- `latency_p95`
- `latency_p99`

The report also includes a markdown summary table for each pattern / transport /
run group so the output remains readable without dropping the canonical
`RESULT,current,...` lines.

## Review Notes

When changing perf code or docs, verify all of the following:

- the edited entrypoint still has working `--help`
- at least one single pattern runs end-to-end
- if multi perf was changed, confirm the touched pattern runs end-to-end before treating the result as policy-complete
- the documented scope matches the patterns actually supported by the scripts
- no new wrapper hides the benchmarked hot path
