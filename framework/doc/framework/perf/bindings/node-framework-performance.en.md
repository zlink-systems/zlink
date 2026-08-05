# Node Framework Performance Plan

> Common policy: [ZLink Framework Performance Policy](../README.en.md)
>
> Applies to: `framework/languages/node`

## 1. Purpose

Node framework perf mixes together the cost of the event loop, the native addon, and the
TypeScript/JavaScript wrapper. So the runner clearly records the native addon build/runtime path
and the event loop scheduling conditions in the report.

## 2. Measurement Principles

- Only the Node framework public API is used.
- Native-addon-internal helpers aren't called directly on the benchmark hot path.
- It's a failure if the generated `dist` or `dist-tools` artifact doesn't match the source.
- An event loop turn isn't hidden with a progress-correction timer.
- Callback/event-loop cost is recorded as part of the Node framework's user path.

## 3. Scenario Application

Priority:

1. `client_server_request_reply`
2. `client_server_send`
3. `fanout_publish_1`
4. `stream_send`
5. `spot_to_spot_request_reply`
6. `route_mesh_request_reply`
7. `http_handler_roundtrip`

A scenario with no Node framework implementation yet is left as `unsupported`. Binding perf or raw
native-addon perf isn't run under the same name as a substitute. The priority list is only the
implementation order. The final report must record every scenario of the common policy as
`complete` or `unsupported`.

## 4. Node Metadata

```json
{
  "node_version": "...",
  "v8_version": "...",
  "native_addon_path": "...",
  "typescript_build": "dist",
  "event_loop_policy": "default",
  "npm_script": "..."
}
```

## 5. Runner Location

Recommended location:

```text
framework/languages/node/perf/run_benchmarks.sh
framework/languages/node/perf/results/
```

In a structure with tracked JavaScript artifacts, update the TypeScript source and the tracked JS
together.

## 6. Prohibited

- Don't call the native addon's direct API and report it as a framework result.
- Don't put `setInterval`, a short sleep, or a busy loop in charge of completion progress.
- Don't measure with a stale native addon or stale generated JS.
- Don't arbitrarily correct event loop delay by subtracting it from latency.

## 7. Initial Implementation Order

1. Build the 4KB smoke runner and the common JSON report writer.
2. Connect client-server request/reply and send first.
3. Add the stream and spot scenarios.
4. Add native addon path and generated artifact verification to the runner.
5. Add the full payload matrix.
6. Add the `serial`, `pipelined`, `concurrent` concurrency profiles.
