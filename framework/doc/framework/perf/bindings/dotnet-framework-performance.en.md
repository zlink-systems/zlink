# .NET Framework Performance Plan

> Common policy: [ZLink Framework Performance Policy](../README.en.md)
>
> Applies to: `framework/languages/dotnet`

## 1. Purpose

.NET framework perf records the current canonical framework's user flow as the baseline
performance. The C++/Java/Node frameworks are compared against this document's scenario meaning
and report schema. However, the canonical baseline isn't just left as words — the report records
the commit hash, package version, and assembly path so the same baseline can be re-run.

## 2. Measurement Principles

- Only the `Zlink.Framework` public API is used.
- Framework-internal implementation types aren't called via reflection.
- The benchmark that includes ASP.NET Core hosting cost is kept separate from the benchmark that
  looks only at framework dispatch.
- The JIT, GC, and ReadyToRun configuration are recorded in the report metadata.
- The commit hash, package version, and assembly path are recorded in the report metadata.

## 3. Scenario Application

.NET serves as the meaning baseline for the common scenarios.

Priority:

1. `client_server_request_reply`
2. `client_server_send`
3. `fanout_publish_1`
4. `dealer_mesh_request_reply`
5. `route_mesh_request_reply`
6. `stream_send`
7. `stream_request_reply`
8. `spot_to_spot_request_reply`
9. `spot_to_router_egress`
10. `router_to_spot_ingress`
11. `http_handler_roundtrip`

This list is the implementation order. Since .NET serves as the meaning baseline for the common
scenarios, a common scenario must not be missing from the final report. A scenario with no
measurement runner yet is recorded as `unsupported`, and a finished scenario is recorded as
`complete` or `failed`.

## 4. Benchmark Runtime

Choose one of the two and use it.

| Approach | Use |
|------|------|
| Custom runner | Easier to match the process model and report schema of other language runners |
| BenchmarkDotNet | Better for micro benchmarks and runtime diagnostics |

The official cross-language comparison report must output the common JSON schema. Even when using
BenchmarkDotNet, keep a common-schema conversion step.

## 5. .NET Metadata

```json
{
  "dotnet_version": "...",
  "runtime": "CoreCLR",
  "tiered_compilation": true,
  "ready_to_run": true,
  "gc_mode": "server",
  "benchmark_engine": "custom",
  "package_version": "...",
  "assembly_path": "..."
}
```

## 6. Runner Location

Recommended location:

```text
framework/languages/dotnet/perf/run_benchmarks.sh
framework/languages/dotnet/perf/results/
```

## 7. Prohibited

- Don't call framework-internal runtime types directly via reflection.
- Don't mix ASP.NET Core HTTP results and framework-only dispatch results into the same scenario.
- Don't leave only a BenchmarkDotNet summary and omit the common JSON report.
- Don't record an interrupted result as the canonical .NET baseline.

## 8. Initial Implementation Order

1. Decide the canonical scenario names and the DTO/payload generator.
2. Build the 4KB smoke with the custom runner.
3. Add the ASP.NET Core HTTP handler roundtrip as a separate scenario.
4. Add the full payload matrix and runtime metadata.
5. Add the `serial`, `pipelined`, `concurrent` concurrency profiles.
6. When comparing against C++ fake-backend results, clearly mark the `measurement_layer` and
   `backend` difference.
