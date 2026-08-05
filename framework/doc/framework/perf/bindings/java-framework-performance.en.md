# Java Framework Performance Plan

> Common policy: [ZLink Framework Performance Policy](../README.en.md)
>
> Applies to: `framework/languages/java`

## 1. Purpose

Java framework perf confirms the Spring/Java framework usage flow is measured with the same
meaning as C++, .NET, and Node. Since JVM warmup and GC have an effect, the runner clearly records
the runtime options and warmup conditions in the report.

## 2. Measurement Principles

- Only the Java framework public API is used.
- Framework-internal implementation helpers aren't called directly on the benchmark hot path.
- Even when comparing against the C++ fake backend, Java uses only the real transport or the
  public testkit path, until an official fake/test backend provided by the Java framework exists.
- JVM startup cost isn't included in the measured active window.
- A figure from before warmup finishes isn't used as final evidence.

## 3. Scenario Application

The common scenario names are used as-is. A scenario not yet in the Java framework is recorded as
`unsupported`.

Priority:

1. `client_server_request_reply`
2. `client_server_send`
3. `fanout_publish_1`
4. `stream_send`
5. `spot_to_spot_request_reply`
6. `route_mesh_request_reply`
7. `http_handler_roundtrip`

This list is the implementation order. The final report must record every scenario of the common
policy as `complete` or `unsupported`. A scenario must not be omitted from the report just because
it's not on the priority list.

## 4. JVM Metadata

The Java report records the following metadata on top of the common schema.

```json
{
  "java_version": "...",
  "jvm": "...",
  "jvm_args": ["-server"],
  "gc": "...",
  "heap_initial_mb": 512,
  "heap_max_mb": 512,
  "warmup_mode": "duration"
}
```

Recommended defaults:

- Use the server VM.
- In the full matrix, use a separate process per size.
- Record the heap and GC options in the report.
- Keep the full matrix warmup longer than the smoke warmup for JIT stabilization.

## 5. Runner Location

Recommended location:

```text
framework/languages/java/perf/run_benchmarks.sh
framework/languages/java/perf/results/
```

If a Gradle task is added, the shell runner calls the Gradle task, but the final report schema
keeps the common JSON format.

## 6. Prohibited

- Don't bypass the Java framework and call only the binding/native API directly, reporting it as a
  framework result.
- Don't record a figure with insufficient JIT warmup as a full-matrix result.
- Don't have the runner adjust results to hide a GC pause or timeout.
- Don't compare a C++-only fake-backend result with a Java real-transport result using the same
  `measurement_layer` and `backend` values.

## 7. Initial Implementation Order

1. Build the 4KB smoke runner and the common JSON report writer.
2. Connect client-server request/reply and send first.
3. Add the stream and spot scenarios.
4. Stabilize the JVM metadata and warmup policy.
5. Add the full payload matrix.
6. Add the `serial`, `pipelined`, `concurrent` concurrency profiles.
