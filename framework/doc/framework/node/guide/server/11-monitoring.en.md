---
title: "11. Monitoring — Status Observation And Diagnostics · Node/TypeScript"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: Location](10-location.en.md) | [Next: Operations — metrics · drain · readiness](12-operations.en.md)
<!-- framework-adapter-nav:end -->

# 11. Monitoring — Status Observation And Diagnostics

> **The document that owns this chapter's contract** — covered by the
> [Node.js location and observability public contract](../../../common/spec/server/languages/node/interfaces/03-location-observability.en.md).
> This chapter explains the four observation surfaces that contract exposes, focused on
> usage.

Handler calls alone can't show you all of operations. Whether a connection is ready, which
peer dropped, where a message failed — you also have to read this from the framework
surface. The Node framework exposes this through **two channels** — a status snapshot and
async iterable, and message flow records.

There's no surface that receives a runtime event as a provider handler. Observation always
goes through one of the two channels below.

## 1. Observation Surfaces

| What you're watching | Injection token | Where it's covered |
|---|---|---|
| Host lifecycle (relocate / drain / readiness) | `ZLINK_FRAMEWORK_RUNTIME` | [12. Operations](12-operations.en.md) §6.1 |
| A MeshNode's node / peer / channel readiness | `ZLINK_ROUTE_MESH_RUNTIME` | [12. Operations](12-operations.en.md) §5 |
| A ClientServer channel's target status | `ZLINK_CLIENT_SERVER_RUNTIME` | This chapter §2 |
| A pub/sub channel's publisher status | `ZLINK_FANOUT_RUNTIME` | This chapter §2 |
| Location store status and topology | `ZLINK_LOCATION_RUNTIME_QUERY` | [10. Location](10-location.en.md) §4 |
| Message receive / dispatch / failure and flow | The message flow in `configureDispatch()` | This chapter §3 |

**All of these are received via `@Inject(token)`.** The type alone doesn't tell Nest what
to inject.

## 2. Status Snapshot And Status Stream

Every status surface has the same shape — `snapshot(...)` gives the current value, and
`observe(...)` gives every change after that as an **async iterable**.

```typescript
@Injectable()
export class MeshWatcher {
  constructor(
    @Inject(ZLINK_ROUTE_MESH_RUNTIME) private readonly meshRuntime: ZLinkRouteMeshRuntime
  ) {}

  ready(): boolean {
    const snapshot = this.meshRuntime.snapshot('game.room');  // One current value.
    return this.meshRuntime.isReady('game.room');
  }

  async watch(signal: AbortSignal): Promise<void> {
    // A slow consumer that exceeds capacity skips intermediate values.
    for await (const observed of this.meshRuntime.observe('game.room', 64, signal)) {
      // observed.status is the complete status after the change -- not an event with
      // only the changed fields.
      // observed.loss is how many this subscription has missed.
      this.record(observed.status);
    }
  }
}
```

**To end the subscription, abort the `AbortSignal`.** You can `break` out of the
`for await` loop, but the signal is the only way to stop it from outside.

| | `snapshot(...)` | `observe(...)` |
| --- | --- | --- |
| What it gives | One value, at call time | The complete value on every change |
| When to use it | An operations endpoint response, a one-off check | Recording or reacting to state transitions |
| Can it miss anything | N/A | Skips intermediate values past capacity |

Host status has the same shape.

```typescript
for await (const observed of runtime.observe(signal)) {
  const status = observed.status;
  logger.log(`host lifecycle: ${status.state} ${status.relocationResult}`);

  // How many this subscription missed. Splits out what was skipped by coalescing versus
  // what's gone for good.
  if (observed.loss.discardedTerminalCount > 0n) {
    logger.warn(`lost terminal statuses: ${observed.loss.discardedTerminalCount}`);
  }
}
```

Peer status carries only the Node RID, its current state, and why it's unavailable.
Connection intent, discovery source, and lifecycle generation are framework-internal state
and aren't exposed.

## 3. Message Flow Tracing

Where and how an individual message ended is seen through message flow. `configureDispatch()`
sets the level.

```typescript
builder.configureDispatch()
  .messageFlow(ZLinkMessageFlowLogMode.ErrorsOnly)   // Default -- failures and backpressure only.
  .traceLogFile(`${config.logDir}/flow-${config.instanceName}.log`)
  .traceLabel(config.instanceName);
```

| Level | Recording scope |
| --- | --- |
| `Off` | Records nothing |
| `ErrorsOnly` (default) | Dispatch failures and backpressure |
| `KeyTransitions` | The above + major transitions like receive/dispatch/complete |
| `Verbose` | The above + a record for every individual message |

**The values are PascalCase.** Don't copy `ERRORS_ONLY` straight over from another
language's docs.

**Keep operations at `ErrorsOnly` and raise it only when needed.** `Verbose` records
something for every message, so on high-throughput paths it becomes a load in itself.

To receive records in your program, register an observer.

```typescript
builder.configureDispatch().setMessageFlowObserver(FlowRecorder);
```

Register the observer as a provider class — a class that implements
`ZLinkMessageFlowObserver`, not a function.

## 4. Metrics

> **What the Node runtime actually emits is only part of the contract.** The contract
> defines 47 instruments; `runtime-metrics.ts` declares 44 names, and fewer than that
> actually get recorded. The emission points are scattered across several files, so the
> exact count isn't pinned down. **Check that the instrument you need is actually emitted
> before building a dashboard.**

The contract for instrument names, kinds, units, and labels is owned by
[Runtime Metrics And Aggregation Rules](../../../common/spec/25-runtime-metrics.ko.md).

## 5. Readiness And Liveness

Node has no separate health-check surface. **You judge it from runtime status.**

```typescript
@Controller('healthz')
export class HealthController {
  constructor(
    @Inject(ZLINK_FRAMEWORK_RUNTIME) private readonly runtime: ZLinkFrameworkRuntime
  ) {}

  @Get('ready')
  ready(@Res() res: Response): void {
    res.status(this.runtime.status.isReady ? 200 : 503).send();
  }
}
```

`status` is a **property** — not a call. Its shape differs from `status()` in the other
languages.

**Reflect a dependency that can drop out briefly, like the store connection, only in
readiness.** Put it in liveness and the orchestrator kills the process the moment the store
blips.

## 6. Common Problems

- **The `observe(...)` loop never ends** → pass an `AbortSignal` and abort it. You can
  `break` inside the loop, but the signal is needed to stop it from outside.
- **Some state transitions are missing** → `observe(...)`'s capacity was exceeded and they
  got skipped. Raise the capacity and consume faster.
- **Calling `status()` throws an error** → it's a property. Drop the parentheses.
- **The enum value doesn't match** → Node uses PascalCase (`ErrorsOnly`).
- **The flow record is empty** → the default level is `ErrorsOnly`, so normal flow isn't
  recorded.
- **No metrics show up at all** → that's expected. The Node runtime doesn't emit
  instruments yet (§4).
- **The store blipped briefly but the process restarted** → the store status is in
  liveness.

## 7. Related Documents

- The formal contract: [Node.js location and observability public contract](../../../common/spec/server/languages/node/interfaces/03-location-observability.en.md)
- Metrics and drain/readiness operations: [12. Operations](12-operations.en.md)
- Diagnostics option list: [16. Options](16-options.en.md) §4
- Injection token list: [13. Key Interface Usage Index](13-interface-catalog.en.md) §1
