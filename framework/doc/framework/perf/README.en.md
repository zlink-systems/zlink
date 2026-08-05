# ZLink Framework Performance Policy

This document organizes the performance measurement criteria for the ZLink framework
layer. The targets are the C++, Java, .NET, and Node framework implementations.

The existing `doc/perf/PERF_POLICY.md`, `doc/perf/PERF_SINGLE_TEST_POLICY.md`, and
`doc/perf/PERF_MULTI_TEST_POLICY.md` are the performance measurement criteria for core and
bindings. This document doesn't replace those criteria. Framework performance measurement
measures the framework API, execution flow, and sample configuration the user actually
calls, on top of core and bindings.

## Purpose

The purpose of framework performance measurement is to make it possible to compare
whether each language's framework provides the same feature with a similar meaning. It's
not just about getting a fast number -- you need to be able to tell these three things
apart.

- The cost of core and binding transport itself
- The dispatch, serialization, handler, DI, and monitoring cost the framework adds
- The scheduling, JIT, GC, and event-loop cost the language runtime creates

So a framework performance measurement must always record in its report which layer it
measured.

## Targets

Per-language detail policy lives in the documents below.

- [C++ framework perf](bindings/cpp-framework-performance.ko.md)
- [Java framework perf](bindings/java-framework-performance.ko.md)
- [.NET framework perf](bindings/dotnet-framework-performance.ko.md)
- [Node framework perf](bindings/node-framework-performance.ko.md)

## Common Principles

Framework perf is measured against the public framework API the user calls. Directly
accessing a private backend, native handle, or internal queue purely for benchmarking
produces a number with the framework cost stripped out, so it's never used as formal
performance evidence.

A smoke benchmark must pass before measurement. Since the full matrix can take a long
time, it's turned on explicitly. A smoke result is the minimal verification that the
runner and scenario wiring aren't broken -- the final performance judgment is based on the
full matrix or a completed representative benchmark report.

Build artifacts and the runtime must never be run in a stale state. The runner must print
the actual library, executable, package, and runtime paths and versions it used. In a
language that can detect an artifact older than its source, it must fail immediately.

An aborted benchmark or a report with only some scenarios run is never final performance
evidence. The report must clearly record a `complete`, `partial`, `failed`, or
`unsupported` status. A `partial` result file may be kept, but it's never treated as a
success by the perf gate.

## Measurement Layers

Framework perf distinguishes the following layers.

| Layer | Meaning | Purpose |
|------|------|------|
| core/binding baseline | The transport cost the existing core and binding runners measure | A reference baseline for framework results |
| framework micro | Small-unit cost like handler dispatch, serializer, registry, builder | Checking the framework's internal hot path |
| framework fake backend | Swaps transport for a fake backend and measures only the framework path | Isolating framework overhead |
| real transport e2e | An end-to-end measurement using the real ZLink transport | Judging user-perceived performance |
| sample scenario smoke | A quick check of a representative usage flow through a sample | Checking for regressions and wiring errors |

Since per-language implementation status can differ, not every layer is forced at once.
Even so, the report must always record which layer was measured.

`measurement_layer` and `backend` mean different things. `measurement_layer` indicates
which layer's cost was measured, and `backend` indicates what kind of backend was actually
used to run that layer.

The recommended `measurement_layer` values are as follows.

- `core_binding_baseline`
- `framework_micro`
- `framework_fake_backend`
- `real_transport_e2e`
- `sample_scenario_smoke`

The recommended `backend` values are as follows.

- `zlink`: Uses the real ZLink transport.
- `fake`: Uses a fake backend built for framework testing.
- `in_memory`: Uses an in-process memory backend.
- `none`: No backend, as in a micro benchmark.

## Common Scenario Matrix

The scenarios below must be provided with as close to the same name and meaning as
possible across each language's framework. A scenario not yet implemented is reported as
`unsupported`, not treated as a success.

| Scenario | What it measures |
|----------|-----------|
| `client_server_send` | Sends a one-way message from client to server |
| `client_server_request_reply` | The client sends a request and receives the server's response |
| `fanout_publish_1` | Publishes to 1 subscriber |
| `fanout_publish_n` | Publishes to multiple subscribers |
| `dealer_mesh_request_reply` | Measures request/reply on a dealer mesh |
| `route_mesh_send` | Measures one-way route send on a route mesh |
| `route_mesh_request_reply` | Measures request/reply on a route mesh |
| `stream_send` | Sends a one-way message over the stream path |
| `stream_request_reply` | Measures request/reply on the stream path |
| `bound_session_send` | Sends a one-way message through a bound session |
| `stream_actor_relay` | Measures throughput and latency of the stream actor relay path |
| `spot_to_spot_send` | Sends a one-way message from a spot to another spot |
| `spot_to_spot_request_reply` | Measures request/reply between spots |
| `spot_to_router_egress` | Measures the outgoing path from a spot toward router/channel |
| `router_to_spot_ingress` | Measures the incoming path from router/channel into a spot |
| `http_handler_roundtrip` | Calls an HTTP handler via the framework HTTP client |

## Payload Size

Payload size surfaces different performance bottlenecks. Looking at only 4KB tells you the
state for a medium-sized message, but it's easy to miss framework dispatch cost and
large-message copy cost. The common full matrix uses the sizes below.

- `64B`: a small message, for looking at framework call, dispatch, and scheduling cost
- `1KB`: close to a typical command or small JSON payload
- `4KB`: a representative medium-sized message
- `64KB`: a large message, for looking at the impact of serialization, copy, and
  backpressure

An initial smoke benchmark can use only `4KB`, to wire up every scenario quickly. For a
final performance comparison or regression judgment, the payload size must be recorded
clearly in the report.

## Concurrency Profile

Payload size alone isn't enough for framework perf. Because the framework layer is
affected by handler dispatch, request tracking, actor scheduling, the event loop, the
thread pool, and backpressure, the concurrency condition must be fixed too.

The common full matrix uses the following profiles first.

| Profile | `concurrency` | `in_flight` | Purpose |
|----------|---------------|-------------|------|
| `serial` | 1 | 1 | Looks at a single request's base latency |
| `pipelined` | 1 | 32 | Looks at the cost when the same logical client waits on several requests at once |
| `concurrent` | 16 | 16 | Looks at the cost when multiple clients, actors, or workers call at once |

A smoke benchmark can use only `serial`. A full matrix or a benchmark meant for regression
judgment includes at least `serial` and `pipelined`. `concurrent` matters for seeing
per-runtime scheduling differences, so it's included in the formal comparison report.

## Common Metrics

Every language's runner records at least the following metrics, with the same meaning.

- `throughput_ops_per_sec`
- `latency_p50_us`
- `latency_p95_us`
- `latency_p99_us`
- `error_count`
- `timeout_count`
- `warmup_seconds`
- `duration_seconds`
- `iterations`
- `total_operations`
- `concurrency_profile`
- `concurrency`
- `in_flight`
- `warmup_iterations`
- `payload_bytes`
- `scenario`
- `measurement_layer`
- `backend`
- `status`

Where possible, also record the CPU model, OS, compiler or runtime version, commit hash,
and build type. On a managed runtime, record the GC mode and JIT-related settings.

`concurrency` means the number of logical clients, actors, workers, or request sources
running concurrently. `in_flight` means the number of requests awaiting completion at a
given moment. Even for a one-way send scenario that doesn't wait for a response, the
runner records as `in_flight` however many outstanding operations it caps for internal
backpressure. Without these two values, the same throughput number can mean something
different for comparison purposes.

## Report Format

A runner keeps both a human-readable summary and a machine-readable JSON report. The
JSON report's minimum structure is as follows.

```json
{
  "framework": "zlink",
  "language": "cpp",
  "commit": "unknown",
  "run_id": "unknown",
  "status": "complete",
  "started_at": "2026-06-05T00:00:00Z",
  "ended_at": "2026-06-05T00:00:10Z",
  "host": {
    "os": "unknown",
    "cpu_model": "unknown",
    "cpu_count": 0
  },
  "build": {
    "type": "Release",
    "runtime_path": "path/to/runtime",
    "toolchain": "unknown"
  },
  "runtime": {
    "name": "unknown",
    "version": "unknown",
    "options": []
  },
  "results": [
    {
      "scenario": "client_server_request_reply",
      "measurement_layer": "real_transport_e2e",
      "backend": "zlink",
      "payload_bytes": 4096,
      "concurrency_profile": "serial",
      "concurrency": 1,
      "in_flight": 1,
      "iterations": 0,
      "total_operations": 0,
      "warmup_seconds": 0,
      "warmup_iterations": 0,
      "duration_seconds": 10,
      "throughput_ops_per_sec": 0,
      "latency_p50_us": 0,
      "latency_p95_us": 0,
      "latency_p99_us": 0,
      "error_count": 0,
      "timeout_count": 0,
      "status": "complete"
    }
  ]
}
```

## Runner Placement

Each language's runner defaults to the following location.

- `framework/languages/<language>/perf/run_benchmarks.sh`

A smoke run is the default behavior. The full matrix is turned on via an environment
variable or explicit option -- for example, a name like `FRAMEWORK_PERF_FULL_MATRIX=1`
can be used.

Before running, the runner prints the following.

- The language and framework package path
- The core or binding runtime path used
- The build type
- The scenario and payload list to run
- The report output path

## Status And Exit Code

The runner uses status and exit code consistently, so automation can interpret them.

| Status | Meaning | Perf gate handling |
|------|------|----------------|
| `complete` | Every selected scenario ran to completion | Success |
| `partial` | Aborted, or only partial results exist | Failure |
| `failed` | There was an execution error, timeout, or policy violation | Failure |
| `unsupported` | A scenario not yet implemented for that language | Not a failure |

If a required scenario is `failed` or `partial` in a smoke run, the runner exits non-zero.
If some scenarios are `unsupported` in the full matrix, they're kept in the report but not
treated as a cause for non-zero. However, if `unsupported` remains against the common
completion criteria, that language's perf coverage isn't considered complete.

## Performance Criteria

Framework perf's criteria use bindings perf as the baseline. This baseline doesn't mean
the framework must be faster than bindings, though. Since the framework adds features like
handler dispatch, serialization, routing, and scheduling on top of bindings, the goal is
to confirm the added cost over the bindings baseline is reasonable.

Performance criteria split into two.

| Criterion | Compared against | Purpose |
|------|-----------|------|
| Satisfaction criterion | The framework result versus the bindings baseline | Judges whether framework overhead is within an acceptable range |
| Regression criterion | The current result versus the previous stable framework baseline | Judges whether already-stabilized framework performance has gotten worse |

The initial satisfaction criteria use the values below. These values can be re-tuned per
language once full matrix results accumulate.

| Language | Throughput criterion | Latency criterion |
|------|-----------------|--------------|
| C++ | At least 70% of the bindings baseline | p95 at most 1.5x the bindings baseline |
| .NET | At least 50% of the bindings baseline | p95 at most 2x the bindings baseline |
| Java | At least 50% of the bindings baseline | p95 at most 2x the bindings baseline |
| Node | At least 40% of the bindings baseline | p95 at most 2.5x the bindings baseline |

The regression criterion compares against the previous stable framework baseline. If the
criteria below are exceeded, the perf gate treats it as a failure.

| Item | Failure criterion |
|------|-----------|
| Throughput | Drops by 10% or more |
| p95 latency | Increases by 15% or more |
| p99 latency | Increases by 25% or more |
| Error or timeout | 1 or more occurrences |
| `partial` result | 1 or more occurrences |

A regression judgment is never made from a single run's result alone. Run it at least 3
times under the same conditions and compare the median. The run conditions must be the
same scenario, same payload, same `concurrency_profile`, same `measurement_layer`, and
same `backend`. If these conditions differ, it's treated as a different benchmark even if
the numbers look the same.

## Phased Rollout

1. Start with the C++ framework to settle the runner and report format. Since C++'s
   framework work is still ongoing, keep micro, fake backend, and real e2e together.
2. Java, .NET, and Node first align on the same report schema and scenario names.
3. Once every language has the smoke matrix aligned, expand the full matrix.
4. Once stable numbers accumulate, split the threshold-based regression gate into a
   separate document.

## POSD Criteria

Perf code tends to be written more loosely than product code, but framework perf follows
POSD criteria too. Bypassing the public API for a benchmark, copying the same setup
knowledge into every scenario, or building a helper that hides the measurement layer all
make numbers harder to interpret over the long run.

The common runner and the per-language runners are designed by defining the same concept
in one place and hiding only the per-language differences underneath -- not by piling up
shallow wrappers.
