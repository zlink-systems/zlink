# C++ Framework Performance Plan

> Common policy: [ZLink Framework Performance Policy](../README.en.md)
>
> Applies to: `framework/languages/cpp`

## 1. Purpose

C++ framework perf measures the cost the framework layer adds, in isolation, when it provides the
same user flow as the `.NET` framework through a C++20 API. Since C++'s framework implementation
verification is currently more active, it adds a micro benchmark and a fake-backend benchmark on
top of the common scenarios.

## 2. Measurement Layers

| `measurement_layer` | Purpose | Runner Coverage |
|---------------------|------|-------------|
| `framework_micro` | Confirms internal framework costs such as serializer, envelope, handler registry, DI scope | C++ only |
| `framework_fake_backend` | Confirms framework runtime path cost with no transport | C++ only, priority |
| `real_transport_e2e` | Confirms user-perceived performance based on the real zlink socket | Common |
| `sample_scenario_smoke` | Confirms the Bingo/TicTacToe structure isn't abnormally slow | C++-only smoke |

The fake backend only replaces the transport. The public fluent builder, handler registration,
runtime dispatch, serializer, and monitoring path must go through the real framework path.

## 3. C++ Scenarios

Every common scenario is kept in scope for support. The initial smoke starts with a 4KB payload
and a representative happy path.

| Scenario | Smoke | Full |
|----------|-------|------|
| `client_server_send` | Required | Required |
| `client_server_request_reply` | Required | Required |
| `fanout_publish_1` | Required | Required |
| `fanout_publish_n` | Optional | Required |
| `dealer_mesh_request_reply` | Required | Required |
| `route_mesh_send` | Required | Required |
| `route_mesh_request_reply` | Required | Required |
| `stream_send` | Required | Required |
| `stream_request_reply` | Required | Required |
| `bound_session_send` | Required | Required |
| `stream_actor_relay` | Required | Required |
| `spot_to_spot_send` | Required | Required |
| `spot_to_spot_request_reply` | Required | Required |
| `spot_to_router_egress` | Required | Required |
| `router_to_spot_ingress` | Required | Required |
| `http_handler_roundtrip` | Required | Required |

An item meant to look at a C++-internal bottleneck, like `spot_actor_dispatch`, is kept as a C++
extension scenario rather than a common scenario. Extension scenarios aren't mixed into the common
comparison table — their `measurement_layer` is recorded as `framework_micro` or
`framework_fake_backend`.

## 4. Embedded HTTP Server Gate

Since the C++ framework provides an embedded HTTP server, it keeps an HTTP-server-only performance
gate in addition to `http_handler_roundtrip`. This gate follows the performance criteria of the
spec below.

- [C++ embedded HTTP server spec](../../common/spec/server/languages/cpp/61-embedded-http-server.en.md)

HTTP handler e2e and HTTP server perf have different purposes.

- HTTP handler e2e is public-consumer verification, so it calls via `zlink::http_client`.
- HTTP server perf must subtract the client implementation's cost, so it calls the zlink, Drogon,
  and Oat++ servers all with the same load generator.
- The runner fixes the load generator to a single one, and the report records the tool name,
  version, command line, thread count, connection count, duration, and warmup.

HTTP server perf scenarios:

| Scenario | Payload | Comparison Basis |
|----------|---------|-----------|
| `http_server_empty_route` | 0B | The faster baseline among Drogon/Oat++ equivalent routes |
| `http_server_json_4kb` | 4KB JSON | The faster baseline among Drogon/Oat++ equivalent routes |
| `https_server_json_4kb` | 4KB JSON | The Drogon/Oat++ baseline under the same TLS conditions |
| `http_server_keep_alive` | 1KB JSON | The Drogon/Oat++ baseline under the same keep-alive profile |

The throughput criterion is a drop within 10% of baseline for the plain HTTP route and the JSON
route. The HTTPS JSON route must be within 15% of baseline under the same TLS conditions. It's a
failure if p95 latency worsens beyond 15% of baseline.

This gate's CTest label is `framework-http-perf`. The long full matrix isn't put in the default
regression — first confirm the short smoke and whether a baseline comparison is possible. The final
completion judgment needs a complete report; an interrupted or partial report isn't treated as a
success. A result that mixes different load generators across zlink, Drogon, and Oat++ within the
same report also isn't treated as a success.

## 5. C++ Micro Benchmarks

Micro benchmarks aren't mixed into the final cross-language comparison table. They're for
diagnosing internal C++ framework bottlenecks.

| Benchmark | What It Measures |
|-----------|-----------|
| `envelope_encode_decode` | Envelope header/body encode, decode |
| `serializer_json_roundtrip` | Registered serializer lookup and JSON roundtrip |
| `handler_registry_dispatch` | Typed handler lookup and invocation |
| `di_scope_resolve` | Scoped dependency resolve cost |
| `call_object_submit` | Fluent call object construction and submit cost |
| `monitoring_publish_filter` | Typed monitoring event filter and handler dispatch |
| `http_route_match_validation` | HTTP method/path matching and request validation |
| `http_response_write_empty` | Empty response write and header formatting |
| `http_json_binding_4kb` | 4KB JSON DTO parse and serialize |

Micro benchmarks also prefer the public or contract-level API. When a runtime detail must be called
directly, it's marked as a C++-only diagnostic and kept separate from the common report.

## 6. Payload Size

Uses the common payload sizes as-is.

- 64B
- 1KB
- 4KB
- 64KB

The initial implementation only allows the `4KB` smoke. All 4 sizes must be added before the full
matrix.

## 7. Runner Location

Recommended location:

```text
framework/languages/cpp/perf/run_benchmarks.sh
framework/languages/cpp/perf/src/
framework/languages/cpp/perf/results/
```

The CMake target is managed in `framework/languages/cpp/CMakeLists.txt`. The long full matrix isn't
put into CTest — only the short smoke is wired to a label.

Recommended CTest labels:

```text
framework-perf
framework-perf-smoke
framework-perf-cpp
framework-http-perf
```

## 8. Artifact Verification

The runner confirms at least the following.

- `framework/languages/cpp/build` exists.
- The benchmark binary isn't older than the source.
- The real transport benchmark uses a framework artifact linked against the current core runtime.
- If a stale artifact is suspected, it fails and demands a rebuild.

## 9. Report Metadata

The C++ report can add the following metadata on top of the common schema.

```json
{
  "cpp_standard": "c++20",
  "build_type": "Release",
  "measurement_layer": "framework_fake_backend",
  "backend": "fake",
  "compiler": "gcc",
  "compiler_version": "..."
}
```

The `measurement_layer` value uses the common policy's values. Even for a C++ extension scenario,
one of `framework_micro`, `framework_fake_backend`, `real_transport_e2e`,
`sample_scenario_smoke` is used. The `backend` value indicates the execution backend, preferring
one of `zlink`, `fake`, `in_memory`, `none`.

The HTTP server perf gate report adds the following fields.

```json
{
  "baseline_framework": "drogon",
  "baseline_version": "unknown",
  "baseline_commit": "unknown",
  "load_generator": "unknown",
  "connection_count": 0,
  "thread_count": 0,
  "tls": false
}
```

## 10. Prohibited

- Don't create a benchmark-only public shortcut API.
- Don't bypass the framework dispatch path in a fake-backend benchmark.
- Don't call the C API directly on the hot path and report it as a framework figure.
- Don't record an interrupted result as a C++ framework perf state.
- Don't judge framework performance sufficient based on sample success alone.
- Don't judge the HTTP server perf gate using `zlink::http_client` figures alone.

## 11. Initial Implementation Order

1. Build the `4KB` smoke runner and the JSON report writer.
2. Connect fake-backend channel request/reply, route request/reply, stream send, and spot actor
   dispatch first.
3. Use `zlink::http_client` for the HTTP handler roundtrip.
4. Add micro benchmarks to isolate internal framework costs.
5. Add the embedded HTTP server perf smoke and the Drogon/Oat++ baseline fixture.
6. Add the 4 payload sizes and the full matrix.
7. Add the `serial`, `pipelined`, `concurrent` concurrency profiles.
8. Generate the comparison table with the same schema as the Java, .NET, and Node reports.
