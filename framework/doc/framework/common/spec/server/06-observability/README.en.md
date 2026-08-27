---
title: "Observability"
---

# Observability

[Spec table of contents](../README.en.md) · [Next: 01. Runtime Status And Operational Diagnostics](01-runtime-monitoring.en.md)

## 1. What Can Be Observed

The application and the operator observe what one process is doing right
now, at four layers, using this topic's four documents. The complete
state at this moment (whether the Host can accept new work, and whether
[RouteMesh](../00-foundation/02-glossary.en.md#routemesh) — the scope in
which multiple nodes exchange Channel messages — as well as
ClientServer and automatic fanout are each ready) is confirmed by query,
and the moment that state changes is observed via
a change stream. Throughput, wait, failure, and current counts that
accumulate over time are collected as metrics, and how far one message
got in processing, and which other message it continued from the same
cause as, are traced with trace and the two identifiers.

The four documents each own a different observation unit and don't
overlap — the ownership boundary per observation unit is defined by
[§2](#2-documents-in-this-topic). These records don't change message
delivery/completion guarantees, routing, handler distribution, or
lifecycle decisions. The application, operational tooling, and the
provider each intervene differently.

| Party | What it does in this topic |
|---|---|
| Application | Configures the record level (diagnostics level), sampling ratio, and whether to record message size, and queries or observes status by registered name. |
| Framework | Builds status/metric/trace records at each processing boundary and delivers them through standard paths. Creates, propagates, and restores correlation/flow identifiers. |
| Provider | Configures the structured logger, metric exporter, and telemetry provider. Delaying processing or changing the result is forbidden. |
| Operational tooling | Doesn't intervene inside an unregistered process, and judges process state using only public query/subscription and structured log. |

## 2. Documents In This Topic

| Document in this topic | Ownership scope |
|---|---|
| [01. Runtime Status And Operational Diagnostics](01-runtime-monitoring.en.md) | The complete status at a specific point in time, the status-change stream, and structured-log identifiers |
| [02. Runtime Metrics](02-runtime-metrics.en.md) | The name/kind/unit/label of metrics accumulated and collected over time |
| [03. Message Flow Tracing](03-message-flow-tracing.en.md) | The progress record (trace) of one message and its attributes and record level |
| [04. Request Correlation](04-flow-correlation.en.md) | The creation, format, propagation, ownership, and lifetime of `correlation_id`, `flow_id`, `flow_origin` |

The results of individual host operations (relocation, shutdown) aren't
owned by this topic but by
[Host Relocation And Shutdown](../05-location-relocation/05-host-relocation-flow.en.md).

## 3. Find By Question

| Question | Section with the answer |
|---|---|
| How does an operator confirm at once whether the whole process can accept new work right now | [01. Runtime Status And Operational Diagnostics "3. Host State — Values Read At Once"](01-runtime-monitoring.en.md#3-host-state--values-read-at-once) |
| What confirms the readiness of each of RouteMesh, ClientServer, and automatic fanout | [01. Runtime Status And Operational Diagnostics "5. Topology State — RouteMesh, ClientServer, Automatic Fanout"](01-runtime-monitoring.en.md#5-topology-state--routemesh-clientserver-automatic-fanout) |
| What's observed to not miss the moment state changes, and what happens if the observer is slow | [01. Runtime Status And Operational Diagnostics "6. Observing State Changes — Sequence And The Complete Status"](01-runtime-monitoring.en.md#6-observing-state-changes--sequence-and-the-complete-status) · ["7. When The Observer Is Slow — Source, Coalescing, And The Lost-Update Count"](01-runtime-monitoring.en.md#7-when-the-observer-is-slow--source-coalescing-and-the-lost-update-count) |
| How to query where this Actor/Spot is right now using an operational tool | [01. Runtime Status And Operational Diagnostics "8. Querying An Object's Current Location"](01-runtime-monitoring.en.md#8-querying-an-objects-current-location) |
| Where to find why a state changed | [01. Runtime Status And Operational Diagnostics "9. Structured Log"](01-runtime-monitoring.en.md#9-structured-log) |
| What names of numbers to collect to see throughput/wait/failure/current counts on a dashboard | [02. Runtime Metrics "2. Naming And Aggregation Rules"](02-runtime-metrics.en.md#2-naming-and-aggregation-rules) |
| What guarantees the same instrument can be seen on the same dashboard/alert in every language | [02. Runtime Metrics "2. Naming And Aggregation Rules"](02-runtime-metrics.en.md#2-naming-and-aggregation-rules) · ["10. Label Cardinality"](02-runtime-metrics.en.md#10-label-cardinality) |
| What numbers show how long one relocation took and where it stalled | [02. Runtime Metrics "8. Host Relocation And Shutdown"](02-runtime-metrics.en.md#8-host-relocation-and-shutdown) |
| How to trace how far one message got in processing and where it failed | [03. Message Flow Tracing "2. Which Processing Stages Are Recorded"](03-message-flow-tracing.en.md#2-which-processing-stages-are-recorded) |
| What it costs to turn this flow record on and off, and whether it's really zero cost when off | [03. Message Flow Tracing "5. Changing The Record Level At Runtime And The Cost Rule"](03-message-flow-tracing.en.md#5-changing-the-record-level-at-runtime-and-the-cost-rule) |
| What links a request and its reply | [04. Request Correlation "2. The Role Of The Two Identifiers"](04-flow-correlation.en.md#2-the-role-of-the-two-identifiers) |
| How to know several messages started from the same cause | [04. Request Correlation "5. Propagation Rule"](04-flow-correlation.en.md#5-propagation-rule) |
| Whether personal information or payload goes into these identifiers | [04. Request Correlation "8. Observability And Privacy"](04-flow-correlation.en.md#8-observability-and-privacy) |
| What an operator turns on and reads first when investigating an intermittent failure | [§4 The Order For Chasing An Intermittent Failure](#4-the-order-for-chasing-an-intermittent-failure) |

## 4. The Order For Chasing An Intermittent Failure

When chasing an intermittent failure, **turn on and read the message
tracking and file log that already exist, first.** Adding a new
temporary log and repeating reproduction is forbidden. That approach
forces you to rerun the whole reproduction cycle just to see one
exception, and it misses the cause even when it's already stamped in the
existing log.

### 4.1 What To Turn On First

| Target | How to turn it on |
|---|---|
| Message flow (full-span tracking including `flow`, `corr`) | The runtime diagnostics' message flow mode |
| C++ / .NET spot discovery trace | `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY` |
| Java / Kotlin stream trace | `ZLINK_JAVA_STREAM_TRACE=1` |
| Keeping sample server logs | .NET `ZLINK_SAMPLE_EVIDENCE_DIR`, JVM `ZLINK_SAMPLE_KEEP_RUN_DIR=1`, Node is automatic on failure |

If a sample fails intermittently, keep the server log **from the first
reproduction.** A reproduction run without a log leaves only the fact of
failure, not the cause, so that cycle is wasted.

### 4.2 How To Read It

First, line up the normal and failed cases side by side using `flow` and
find **where the transition breaks.** `flow` is the only value that
joins one message across process boundaries. Treating trace kinds as
noise and filtering them out of grep skips right past the line with the
cause.

### 4.3 A Failure Is Always Left In The Flow

Don't build a termination that only returns an error kind to the
application and discards the cause. A failure that leaves no cause can
only be tracked by reproduction, and the reproduction cycle itself
becomes the cost of investigation. Record a termination like a failure,
rejection, or abort with `outcome=failed` and `reason`, as defined by
[03. Message Flow Tracing](03-message-flow-tracing.en.md), carrying a
cause-description string within the implementation-defined length
limit, **under the same `flow` as the message that produced that
failure.**

The cost rule for turning this record on and off is defined by
[03. Message Flow Tracing "5. Changing The Record Level At Runtime And The Cost Rule"](03-message-flow-tracing.en.md#5-changing-the-record-level-at-runtime-and-the-cost-rule).

## 5. What This Topic Does Not Define

| Content | Owning document |
|---|---|
| The progress and result of individual host operations (relocation, shutdown) | [Host Relocation And Shutdown](../05-location-relocation/05-host-relocation-flow.en.md) |
| The ownership and size of metadata the application sends with a message | [Message Model](../00-foundation/05-message-model.en.md) |
| Transport connection liveness and peer deadline | [Transport Connection Liveness](../02-channel-transport/05-transport-liveness.en.md) |
| The definition of the STREAM connection close reason (`close_reason`) | [Session "STREAM Server Session"](../04-session/01-stream-session.en.md) |

---

[Spec table of contents](../README.en.md) · [Next: 01. Runtime Status And Operational Diagnostics](01-runtime-monitoring.en.md)
