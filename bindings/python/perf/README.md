# Python PERF

Policy-aligned perf suite for `bindings/python`.

This tree is the official Python binding performance surface. It must stay
aligned with:

- `bindings/README.md` perf policy
- `doc/perf/PERF_POLICY.md`
- `doc/perf/PERF_SINGLE_TEST_POLICY.md`
- `doc/perf/PERF_MULTI_TEST_POLICY.md`

The goal is to measure Python binding boundary cost on the canonical public
surface, not to hide extra work behind helper wrappers.

## Entrypoints

- `./perf/run_benchmarks.sh`
- `./perf/run_benchmarks_multi.sh`
- `./perf/single/run_benchmarks.sh`
- `./perf/multi/run_benchmarks.sh`

Top-level wrappers must stay thin and forward directly to the suite-specific
runner so the documented execution path matches the real measurement path.

## Layout

- `perf/single/`
- `perf/multi/`
- `perf/run_benchmarks.sh`
- `perf/run_benchmarks_multi.sh`

Each pattern keeps its own executable source file. This preserves visible
ownership of the hot path and avoids a shallow mega-runner that obscures
pattern-specific costs.

## Single Suite

Single-suite measurements use the recv path only and expose the official
shared CLI:

- `--pattern`
- `--duration`
- `--msg-sizes`
- `--transports`
- `--runs`
- `--results-dir`
- `--results-tag`

Patterns:

- `PAIR`
- `PUBSUB`
- `DEALER_DEALER`
- `DEALER_ROUTER`
- `ROUTER_ROUTER`

Policy transport matrix:

- `PAIR`: `tcp`, `tls`, `ws`, `wss`, `inproc`, `ipc`
- `PUBSUB`: `tcp`, `tls`, `ws`, `wss`, `inproc`, `ipc`
- `DEALER_DEALER`: `tcp`, `tls`, `ws`, `wss`, `inproc`, `ipc`
- `DEALER_ROUTER`: `tcp`, `tls`, `ws`, `wss`, `inproc`, `ipc`
- `ROUTER_ROUTER`: `tcp`, `tls`, `ws`, `wss`, `inproc`, `ipc`

When a selected combination is outside the policy transport matrix, or the
runtime reports `protocol not supported`, the runner emits
`UNSUPPORTED,current,...` for that case. Other execution failures remain `fail`
so regressions are not hidden as unsupported cases.

## Multi Suite

The multi suite is process-isolated. The policy transport matrix for the multi
suite is `tcp`, `tls`, `ws`, `wss`. Combinations outside that matrix, or
runtimes that report `protocol not supported`, are reported as
`UNSUPPORTED,current,...`; other failures remain `fail`.

The multi runner exposes the same common CLI surface plus `--clients`.
Python multi defaults server and client context I/O threads to `4`, matching
the current C multi baseline resource profile.

Patterns:

- `MULTI_DEALER_DEALER`
- `MULTI_DEALER_ROUTER`
- `MULTI_ROUTER_ROUTER`
- `MULTI_PUBSUB`
- `MULTI_STREAM`

Shared component contract:

- `MULTI_STREAM` client uses the shared core `perf_stream_client` path required
  by the perf policy and execution guide. Python sets
  `--completion-wait-ms` to `PERF_MULTI_STREAM_COMPLETION_WAIT_MS`, then
  `PERF_STREAM_COMPLETION_WAIT_MS`, then `10000` so the shared client can wait
  for the slower public Python stream server's in-flight replies after the
  active window.

## Cost Model Rules

Python perf must measure the canonical binding path, so hot paths should avoid:

- hidden payload copies
- hidden list rebuilding beyond multipart shape requirements
- unnecessary UTF-8 encoding or decoding
- helper-specific fallback behavior that the real API does not use
- benchmark-only wrappers that materially change ownership or receive shape

Fastpath helpers may exist for verification, but they must stay clearly
separated from the default canonical perf path.

## Output

Each runnable pattern prints official-style:

```text
RESULT,current,...
```

lines so the output can be consumed by the same reporting flow as other binding
perf suites.

Runner output and saved reports include:

- `## Effective Options (start)`
- `RESULT,current,...` lines
- a markdown summary table
- result files under `results/{single|multi}/report/`
- filenames shaped as `perf_<lang>_<suite>_<platform>_YYYYMMDD_HHMMSS[_<tag>].txt`

## Smoke

Perf requires the approved Core or wheel runtime to be supplied explicitly.
The runner prints its resolved path and SHA-256 and does not fall back to the
repository build directory.

```bash
ZLINK_LIBRARY_PATH=/absolute/path/to/libzlink.so \
  ./perf/run_benchmarks.sh --smoke --pattern PAIR --msg-sizes 64 \
  --transports inproc --duration 1 --runs 1

ZLINK_LIBRARY_PATH=/absolute/path/to/libzlink.so \
  ./perf/run_benchmarks_multi.sh --smoke --pattern DEALER_ROUTER \
  --msg-sizes 64 --transports tcp --clients 1 --duration 1 --runs 1
```

Smoke mode checks lifecycle, required `RESULT` rows and exit status. It does
not write an official report.
