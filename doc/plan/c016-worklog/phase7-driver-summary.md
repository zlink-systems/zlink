# Phase 7 binding perf smoke driver

Artifacts: `phase7-smoke.sh` drives the exact Phase 7 single and multi smoke
matrices; `phase7-results.md` and `phase7-smoke.log` are created on first run
beside it.  The driver never writes to the checkout: every runner receives an
external `--results-dir` under `phase7-run/`.

## Contract implemented

- `--only c,cpp,dotnet,go,java,node,python,rust` selects language functions.
  An existing language block ending in `- overall: PASS` is resumed/skipped.
- Every actual perf command preserves the plan options: `--pattern ALL`, 1024
  bytes, `--duration 1`, `--runs 1`; multi adds `--clients 1`; Go, Python, and
  Rust add `--smoke`.  Each process is wrapped by `timeout 1200` and a timeout,
  exit failure, report failure/skip, incomplete accounting, or missing absolute
  runtime identity is unexpected.  Later languages still run.
- `ALL` expands to seven patterns for every current runner.  Thus expected
  smoke cells are `7 * 6 = 42` for single and `7 * 4 = 28` for multi.  The
  table written to results is per runner: expected, pass, manifest-matched
  unsupported, unexpected, report path, and runner-emitted runtime identity.
  It deliberately has no numeric performance threshold (D-026).
- Runtime identity is parsed only from runner output/report (`META,core_*`,
  `Perf runtime libzlink`, and runner SHA-256 lines).  The driver does not hash
  a library itself.  A missing absolute runtime path is an unexpected result.

## Current CLI check

I ran `--help` once for every non-Node top-level runner while preparing this
driver; all plan option names are accepted, including `--smoke` for Go, Python,
and Rust.  No command substitution was needed.  Node was not run here because
its wrapper can synchronize/build generated tools before forwarding help
(`bindings/node/perf/single/run_benchmarks.sh:95-155` and
`bindings/node/perf/multi/run_benchmarks.sh:95-155`).  At execution time the
driver calls every actual non-Node top-level runner's `--help` once, verifies
the plan options it needs, then calls that runner's smoke command.  Node gets
one total build-capable help preflight through its single wrapper; common CLI
flags are checked there and multi `--clients` is source-confirmed from
`bindings/node/perf/multi/run_benchmarks.ts:62-100`.  No Node help was
performed during this read-only investigation.

## Report-path evidence

| binding | single report determination | multi report determination |
| --- | --- | --- |
| C | `bindings/c/perf/run_benchmarks.sh:725` assigns `<results>/single/report/<name>.txt`; it passes it to the comparison runner at `:949`. | Help documents `<results>/multi/report/perf_c_multi_*.txt` at `bindings/c/perf/run_benchmarks_multi.sh:604-605`. |
| C++ | `bindings/cpp/perf/run_binding_single.sh:469` assigns `<results>/single/report/<name>.txt`; `:658` passes it on. | Help documents `<results>/multi/report/perf_cpp_multi_*.txt` at `bindings/cpp/perf/run_binding_multi.sh:306-307`. |
| .NET | `bindings/dotnet/perf/single/run_benchmarks.sh:653` assigns `REPORT`; `:666` passes `--result-file`. | `bindings/dotnet/perf/multi/run_benchmarks.sh:1440` assigns `REPORT`; `:2186` emits the saved path. |
| Go | Normal mode assigns `<results>/single/report/perf_go_*.txt` at `bindings/go/perf/run_benchmarks.sh:439-442`; smoke replaces it with temporary `smoke-report.txt` at `:450-452`, then prints it at `:806`. | Same structure at `bindings/go/perf/run_benchmarks_multi.sh:665-678`, saved line `:1614`. |
| Java/Kotlin | The runner writes the supplied `report_path` after its `Saved result file` line (`bindings/java/perf/single/run_benchmarks.sh:864-867`); the default prefix is initialized at `:356`. | Equivalent write is `bindings/java/perf/multi/run_benchmarks.sh:1850-1853`; result root is set at `:61`. |
| Node | `bindings/node/perf/single/run_benchmarks.ts:571-580` creates `<results>/single/report` and emits the exact file. | `bindings/node/perf/multi/run_benchmarks.ts:543-552` does the same for multi. |
| Python | `bindings/python/perf/single/run_benchmarks.py:561-571` builds, prints, and writes `report_path`. | `bindings/python/perf/multi/run_benchmarks.py:1620-1630` does likewise. |
| Rust | Normal report path is assigned at `bindings/rust/perf/run_benchmarks.sh:257-258` and passed at `:449`; `--smoke` exits first with `SMOKE PASS` at `:442`. | Equivalent normal path/pass locations are `bindings/rust/perf/run_benchmarks_multi.sh:254-255`, `:1171`, and `:1127`. |

## QUESTIONS

1. The task calls for a single Node `--help` preflight, while it also forbids
   build execution during this investigation and the Node wrapper may build
   before help.  No Node help was run here.  The driver has one literal help
   preflight total, through Node single; Node multi's extra `--clients` option
   is verified from its source before execution.
2. Go and Rust `--smoke` do not guarantee a persistent report file: Go routes
   it through a temporary `smoke-report.txt`; Rust exits before its normal
   report writer.  The driver records those facts and parses durable captured
   stdout.  If Phase 7 requires report files for smoke, those runners need a
   runner-contract change before the driver can truthfully require one.
