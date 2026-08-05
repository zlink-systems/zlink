---
title: "11. Monitoring — Status Observation And Diagnostics · C++"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: Location](10-location.en.md) | [Next: Operations — metrics · drain · readiness](12-operations.en.md)
<!-- framework-adapter-nav:end -->

# 11. Monitoring — Status Observation And Diagnostics

> **The document that owns this chapter's contract** — covered by
> [C++ monitoring public contract](../../../common/spec/server/languages/cpp/interfaces/08-monitoring.en.md).
> This chapter explains the four observation surfaces that contract exposes, focused on
> usage.

Handler calls alone can't show you all of operations. Whether a connection is ready, which
peer dropped, where a message failed — you also have to read this from the framework
surface. The C++ framework exposes this through **four channels** — a status snapshot and
observation, message flow tracing, health check, and a standard logging provider.

There's no surface that receives a runtime event as a DI handler. Observation always goes
through one of the four channels below.

## 1. Observation Surfaces

| What you're watching | Surface | Where it's covered |
|---|---|---|
| Host lifecycle (relocate/drain/readiness) | `framework_runtime_t::status ()` · `observe (...)` | [12. Operations](12-operations.en.md) §6.1 |
| A MeshNode's node/peer/channel readiness | `route_mesh_runtime_t::snapshot (...)` · `observe (...)` | [12. Operations](12-operations.en.md) §5 |
| Location store status and topology | `location_runtime_query_t` | [10. Location](10-location.en.md) §4 |
| Message receive/dispatch/failure and flow | The message flow in `configure_dispatch ()` | This chapter §3 |
| Readiness / liveness judgment | `health_report_t` | This chapter §4 |
| Numbers like CCU and queue depth | The standard logging provider and metric convention | [12. Operations](12-operations.en.md) §1 |

The four channels are consumed differently. Use the **status surface** to read the current
value or receive changes in order; use **message flow** to trace where and how an individual
message ended; use **health** to attach to a load balancer or orchestrator; use the
**logging provider** to export all three outward.

## 2. Status Snapshot And Observation

Every status surface has the same shape — `snapshot(...)` gives one immutable snapshot,
and `observe(...)` gives every change after that, in order.

```cpp
// One snapshot of the current value.
auto snapshot = mesh_runtime.snapshot ("game.room");
const bool ready = mesh_runtime.is_ready ("game.room");

// A change stream. A slow observer that exceeds capacity gets skipped.
auto observation = mesh_runtime.observe (
  "game.room", 64, [] (const mesh_node_snapshot_t &next) {
      // The complete snapshot after the change arrives — not an event with only the
      // changed fields.
      record (next);
  });
```

**The observation object is the subscription's lifetime.** The callback only fires while
you keep alive the `std::unique_ptr<mesh_runtime_observation_t>` that `observe(...)`
returns. Drop it and the subscription ends. Bind it to a local and forget it, and the
subscription disappears right there.

The callback **runs on the runtime thread.** Don't make a blocking call inside it or call
back into a framework surface. Pull out the value, hand it to your own data structure, and
return immediately.

| | `snapshot (...)` | `observe (...)` |
| --- | --- | --- |
| What it gives | One value, at call time | The complete value on every change |
| When to use it | An operations endpoint response, a one-off check | Recording or reacting to state transitions |
| Can it miss anything | N/A | Skips intermediate values past capacity |

Peer status carries only the Node RID, its current state, and why it's unavailable.
Reconnect-attempt counts or internal socket state aren't part of the public contract.

## 3. Message Flow Tracing

Where and how an individual message ended is seen through message flow. `configure_dispatch ()`
sets the level.

```cpp
options.configure_dispatch ()
  .message_flow (message_flow_log_mode_t::errors_only) // Default -- errors and backpressure only.
  .trace_sample_rate (1.0)                            // Sampling ratio.
  .include_message_sizes (true)                       // Also records payload byte size.
  .trace_log_file ("logs/flow.jsonl");                // Written separately from app logs.
```

| Level | Recording scope |
| --- | --- |
| `off` | Records nothing |
| `errors_only` (default) | Dispatch failures and backpressure |
| `key_transitions` | The above + major transitions like receive/dispatch/complete |
| `verbose` | The above + a record for every individual message |

**Keep operations at `errors_only` and raise it only when needed.** `verbose` records
something for every message, so on high-throughput paths it becomes a load in itself.

To receive records in your program, register an observer.

```cpp
class flow_recorder_t : public message_flow_observer_t
{
  public:
    void on_message_flow (const message_flow_event_t &event) override
    {
        // Hand outcome / surface / packet_name / flow_id, etc. to your own storage.
        // This callback also runs on the runtime thread -- no blocking.
        _sink.append (event.outcome, event.packet_name.value_or ("-"));
    }
};

options.configure_dispatch ().set_message_flow_observer (
  std::make_shared<flow_recorder_t> (sink));

// For a short record, you can pass a single function instead of a class.
options.configure_dispatch ().set_message_flow_observer (
  [&sink] (const message_flow_event_t &event) { sink.append (event.outcome); });
```

`flow_id` and `flow_origin` are the identifiers that tie together the pieces of one
request as it crosses multiple nodes. The correlation rules are owned by
[Flow Correlation](../../../common/spec/27-flow-correlation.ko.md).

## 4. Health Check

Readiness and liveness are both judged from a single `health_report_t`. If you use HTTP
hosting, wire it straight to an endpoint.

```cpp
options.http ()
  .listen ("http://0.0.0.0:8080")
  .map_readiness ("/healthz/ready")   // report.ready ()  -- readiness != unhealthy
  .map_liveness ("/healthz/live");    // report.live ()   -- liveness  != unhealthy
```

| Verdict | Meaning | Load balancer behavior |
| --- | --- | --- |
| `healthy` | Normal | Sends traffic |
| `degraded` | Some capability is down but it still processes | Keeps sending |
| `unhealthy` | Can't process | Removed from the target set |

**Register by scope.** `readiness` asks "can it take traffic right now," `liveness` asks
"does the process need restarting." A dependency that can drop out temporarily, like the
Location store connection, belongs only in readiness — put it in liveness and the
orchestrator kills the process the moment the store blips.

## 5. Structured Logging

Runtime state changes and diagnostics go out through the standard logging provider.
Provider configuration is covered in [19. Configuration](19-configuration.ko.md).

Some things are **not** part of the public contract. Don't write code that consumes these
directly.

- Raw event DTOs per socket / Spot / Actor / STREAM
- Raw event handler and source-registration builders
- Metric sample DTOs and application callbacks
- Exporter lifecycle, registry, and provider internal state

Metric names, kinds, units, and labels are owned by
[Runtime Metrics And Aggregation Rules](../../../common/spec/25-runtime-metrics.ko.md).

## 6. Common Problems

- **I registered `observe(...)` but the callback never fires** → you likely dropped the
  returned observation object. That object is the subscription's lifetime. Keep it as a
  member.
- **There's a deadlock inside the callback** → it runs on the runtime thread. Don't call
  back into a framework surface or block-wait inside it.
- **Some state transitions are missing** → `observe(...)`'s capacity was exceeded and they
  got skipped. If you need every transition, raise the capacity and make the callback
  return faster.
- **The flow record is empty** → the default level is `errors_only`, so normal flow isn't
  recorded. Raise it to `key_transitions` or above.
- **The store blipped briefly but the process restarted** → the store dependency is in
  liveness. Move it to readiness.

## 7. Related Documents

- The formal contract: [C++ monitoring public contract](../../../common/spec/server/languages/cpp/interfaces/08-monitoring.en.md)
- Metrics and drain/readiness operations: [12. Operations](12-operations.en.md)
- Logging provider configuration: [19. Configuration](19-configuration.ko.md)
- HTTP endpoint registration: [20. HTTP Hosting](20-http-hosting.ko.md)
