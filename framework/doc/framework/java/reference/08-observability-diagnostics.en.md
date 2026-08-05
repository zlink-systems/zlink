# 08. Observability diagnostics

[Reference index](README.en.md)

This category covers `ZLinkDispatchOptions`/`ZLinkDiagnosticsOptions`, which configure the
trace/metric/log recording level; `ZLinkFrameworkRuntime` (shared with the host-lifecycle
category), which reads host/topology status; and the `ZLinkFrameworkErrorKind` correspondence
table used to judge failures across every category. The exact signatures are owned by the
[Java configuration and host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.en.md)
and the
[Java common runtime exact interface](../../common/spec/server/languages/java/interfaces/common-runtime.en.md)
(Korean-only).

---

## `configureDispatch().diagnostics()` (configuration time)

Sets the trace/metric recording level and sampling.

```java
options.configureDispatch()
    .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
    .traceSampleRate(0.1)
    .includeMessageSizes(true);
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.messageFlow(ZLinkMessageFlowLogMode)` | Implementation default | The detail level to record: one of `OFF`/`ERRORS_ONLY`/`KEY_TRANSITIONS`/`VERBOSE`/`DIAGNOSTIC` |
| `.traceSampleRate(double)` | Implementation default | `0.0`..`1.0`. Out of range is a configuration error |
| `.includeMessageSizes(boolean)` | `false` | Whether to include the payload size distribution in telemetry. The payload content itself is never recorded |
| `.traceLogFile(path)` | None | The file path to leave diagnostics records in |
| `.traceLabel(id)` | None | The label to attach to diagnostics records |
| `.setMessageFlowObserver(observerType)` / `.setMessageFlowObserver(ZLinkMessageFlowObserver)` | None | Registers an observer that receives `ZLinkMessageFlowEvent` |

Each modifier is a synchronous fluent call returning `ZLinkDispatchOptions` — not a registration
with no return value. `ZLinkUnhandledDispatchOptions`, which `unhandled()` returns, also
configures the handling policy for a request/send/publish with no handler
(`REPLY_ERROR`/`LOG_AND_DROP`/`DROP`/`THROW`) on this same builder.

**Completion result.** The application configures the trace/metric/log recording destination
(exporter, remote backend) separately (e.g. a `ZLinkMetricsCustomizer` bean that configures a
Micrometer `MeterRegistry`).

**When to use.** Use this to set the default recording level at startup.

---

## `ZLinkFrameworkRuntime.status` / `observe` (read/observe)

Queries or observes the host-wide status (lifecycle state, relocation/termination results,
inbound dispatch backpressure). The exact signature and example are owned by the `status`/
`observe` entry in the host-lifecycle category.

**When to use.** Use `ZLinkFrameworkRuntimeStatus.inboundDispatch()`
(`ZLinkInboundDispatchStatus`) to check application HWM usage and backpressure status. Use the
status-query entry in the topology-discovery category for a specific MeshName/ChannelName's
availability.

---

## `ZLinkFrameworkErrorKind` correspondence table

When a Framework operation fails, `ZLinkFrameworkException.kind()` tells you the cause family.
This table is the shared basis for the completion-kind descriptions in every category.

| Kind | What the application should check |
| --- | --- |
| `NOT_FOUND` | Whether the requested Actor, Spot, handler, route, or target exists |
| `ALREADY_EXISTS` | Whether create and registration must be handled idempotently |
| `TYPE_MISMATCH` | Whether the stable type matches the requested application type |
| `NOT_CONFIGURED` | Whether the required role, handler, Store, or object client was registered at startup |
| `REJECTED` | Framework admission, a filter, or a runtime policy without a typed result rejected the operation |
| `UNAVAILABLE` | Whether the target, route, Store, or worker can currently handle the operation |
| `CAPACITY_EXCEEDED` | Whether placement, queue, or a bounded resource has no room left |
| `DEADLINE_EXCEEDED` | The operation did not complete within the deadline. Whether the result had side effects follows that operation's contract |
| `SHUTTING_DOWN` | The runtime is not accepting new admission. Use a different serving instance |
| `PROTOCOL_ERROR` | Whether the protocol or reply contract matches the peer |
| `INVALID_OPERATION` | The requested operation is not allowed in the current object/session/runtime state |
| `DATA_LOST` | A published relocation payload could not be found, or failed validation. There is no arbitrary rollback to the previous owner |
| `INTERNAL_FAILURE` | A Framework failure not expressible by the categories above. Check log and trace correlation information for the cause |

**Completion result.** Only the Framework creates a `ZLinkFrameworkException`, and `value()`
returns a language-neutral common number `0..12` (`fromValue(int)` converts back).
`ZLinkConfigurationException` (a startup validation failure) is a subtype of
`ZLinkFrameworkException`, but a separate layer as a startup-only error. `Message` is a
description for human diagnosis, not a programmatic branching target. This kind does not tell you
whether to retry — the application decides that directly, checking the operation's completion
condition, idempotency, and business state.

**When to use.** Use this table to look back at the kind given in each category entry's
"Completion result" and decide how to respond.

---

See the
[Java configuration and host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.en.md)
and the
[Java common runtime exact interface](../../common/spec/server/languages/java/interfaces/common-runtime.en.md)
(Korean-only) for the full rationale.
