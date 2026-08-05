# Dotnet PERF Common

Shared perf helpers for dotnet bindings.

Current implementation keeps `PerfCommon.cs` in each project (`single` and
`multi`) to preserve project-local builds and avoid extra packaging overhead.
This directory is reserved for small shared helpers or templates that do not
hide the benchmark hot path.

Perf policy reminder:

- do not move pattern-specific send/recv loops here
- do not add generic wrappers that obscure what a benchmark is measuring
- keep reusable code limited to utility concerns that do not change the cost
  model under test
