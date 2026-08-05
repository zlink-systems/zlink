# C++ Binding Perf

C++ binding perf runners verify the hot path exposed by `bindings/cpp/include/zlink`.
They are benchmark and regression surfaces, not alternate public API contracts.

## Entry Points

- Single: `bindings/cpp/perf/run_benchmarks.sh`
- Multi: `bindings/cpp/perf/run_benchmarks_multi.sh`

Each entry point owns its suite directly:

- `run_benchmarks.sh` -> single runner
- `run_benchmarks_multi.sh` -> multi runner

The C++ perf runner is self-contained under `bindings/cpp/perf/` while
preserving the policy-compliant CLI exposed from the C++ perf directory.

Policy references:

- `doc/perf/PERF_POLICY.md`
- `doc/perf/PERF_SINGLE_TEST_POLICY.md`
- `doc/perf/PERF_MULTI_TEST_POLICY.md`

## Policy

- benchmark code must use the canonical C++ binding surface, not raw C helpers
  or transport-specific escape hatches
- non-blocking send paths must rely on the core-provided explicit send result
  contract rather than binding-layer `errno` heuristics
- benchmark helpers must not hide blocking send failures, silently retry, or add
  sleep-based synchronization
- any suspected `core` defect first needs a repository regression under
  `core/tests/`; perf code is a verification surface, not the place to mask the bug
- convenience paths are allowed only when they preserve the same payload shape
  and do not introduce hidden copies or allocations on the canonical hot path

## Quick Start

```bash
# single smoke
bindings/cpp/perf/run_benchmarks.sh \
  --pattern ALL --msg-sizes 64 --reuse-build

# multi smoke
bindings/cpp/perf/run_benchmarks_multi.sh \
  --pattern ALL --msg-sizes 64 --reuse-build
```

## Result Paths

- `bindings/cpp/perf/results/single/report/`
- `bindings/cpp/perf/results/multi/report/`

Report files are generated automatically under `results/{single|multi}/report/`.
`--results-dir` changes the root directory and `--results-tag` appends a suffix
to the report filename.

## TLS Certs

TLS transport uses:

- `bindings/cpp/tests/certs/gen/server.crt`
- `bindings/cpp/tests/certs/gen/server.key`
- `bindings/cpp/tests/certs/gen/ca.crt`
