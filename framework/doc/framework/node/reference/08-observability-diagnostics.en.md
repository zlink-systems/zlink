# 08. Observability diagnostics

[Reference index](README.en.md)

This category covers `ZLinkDispatchOptionsBuilder`/`ZLinkDiagnosticsOptions`, which configure the
trace/metric/log recording level, the `ZLinkFrameworkErrorKind` correspondence table used to
judge failures across every category, and the handler filter contract. The exact signatures are
owned by the
[Foundation types and configuration exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.en.md)
and the
[Location operational query and observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.en.md)
(Korean-only).

---

## `configureDispatch()` (configuration time)

Sets the trace/metric recording level and sampling.

```ts
zlinkFramework().configureDispatch()
  .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
  .traceSampleRate(0.1)
  .includeMessageSizes(true);
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.messageFlow(mode)` | Implementation default | The detail level to record: one of `Off`/`ErrorsOnly`/`KeyTransitions`/`Verbose`/`Diagnostic` |
| `.traceSampleRate(rate)` | Implementation default | `0.0`..`1.0`. Out of range is a configuration error |
| `.includeMessageSizes(include)` | `false` | Whether to include the payload size distribution in telemetry. The payload content itself is never recorded |
| `.traceLogFile(path)` | None | The file path to leave diagnostics records in |
| `.traceLabel(label)` | None | The label to attach to diagnostics records |
| `.setMessageFlowObserver(observerType: Type<ZLinkMessageFlowObserver>)` | None | Registers an observer that receives message-flow events |
| `.setRuntimeErrorSink(sinkType: Type<ZLinkRuntimeErrorSink>)` | None | Registers a sink that receives internal runtime callback/observer errors |

Each modifier is a synchronous fluent call returning `ZLinkDispatchOptionsBuilder` — not a
registration with no return value. The same values can also be specified all at once via
`zlinkFramework().options({ dispatch: { unhandled, diagnostics } })`.

**Completion result.** Registers synchronously with no return value. `unhandled` (the handling
policy for a request/send/publish with no handler: `ReplyError`/`LogAndDrop`/`Drop`/`Throw`)
belongs to the same `ZLinkDispatchOptions`.

**When to use.** Use this to set the default recording level at startup.

---

## `ZLinkHandlerFilterContext` (reading inside a filter)

Reads the dispatch kind and public metadata inside a handler filter (the `filters` entry in the
topology-discovery category).

```ts
async invoke(context: ZLinkHandlerFilterContext, next: ZLinkHandlerFilterNext) {
  if (context.dispatchKind === ZLinkHandlerDispatchKind.ChannelRequest) { ... }
  await next();
}
```

**Options.** Inherits `ZLinkMessageContext` and additionally provides `dispatchKind`
(`NodeDirectSend`/`NodeDirectRequest`/`ChannelSend`/`ChannelRequest`/`ClassicFanout`).
`ChannelSend`/`ChannelRequest` include both RouteMesh and ClientServer. RouteMesh and Node direct
provide `meshName`, while ClientServer and classic fanout do not.

**Completion result.** A read-only property access — there is no separate completion kind.

**When to use.** Use this when a filter must branch on which dispatch path it is on.

---

## `ZLinkFrameworkErrorKind` correspondence table

When a Framework operation fails, `ZLinkFrameworkException.kind` tells you the cause family. This
table is the shared basis for the completion-kind descriptions in every category.

| Kind | What the application should check |
| --- | --- |
| `NotFound` | Whether the requested Actor, Spot, handler, route, or target exists |
| `AlreadyExists` | Whether create and registration must be handled idempotently |
| `TypeMismatch` | Whether the stable type matches the requested application type |
| `NotConfigured` | Whether the required role, handler, Store, or object client was registered at startup |
| `Rejected` | Framework admission, a filter, or a runtime policy without a typed result rejected the operation |
| `Unavailable` | Whether the target, route, Store, or worker can currently handle the operation |
| `CapacityExceeded` | Whether placement, queue, or a bounded resource has no room left |
| `DeadlineExceeded` | The operation did not complete within the deadline. Whether the result had side effects follows that operation's contract |
| `ShuttingDown` | The runtime is not accepting new admission. Use a different serving instance |
| `ProtocolError` | Whether the protocol or reply contract matches the peer |
| `InvalidOperation` | The requested operation is not allowed in the current object/session/runtime state |
| `DataLost` | A published relocation payload could not be found, or failed validation. There is no arbitrary rollback to the previous owner |
| `InternalFailure` | A Framework failure not expressible by the categories above. Check log and trace correlation information for the cause |

**Completion result.** Only the Framework creates a `ZLinkFrameworkException`, and `message` is a
description for human diagnosis, not a programmatic branching target. `ZLinkConfigurationException`
(a startup validation failure) and `TypeError` (an invalid argument) are a different layer from
this kind classification. This kind does not tell you whether to retry — the application decides
that directly, checking the operation's completion condition, idempotency, and business state.

**When to use.** Use this table to look back at the kind given in each category entry's
"Completion result" and decide how to respond.

---

See the
[Foundation types and configuration exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.en.md)
and the
[Location operational query and observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.en.md)
(Korean-only) for the full rationale.
