# 01. Host lifecycle

[Reference index](README.en.md)

This category covers the `ZLinkModule`/`zlinkFramework()` registration entry points and the entry
points `ZLinkFrameworkRuntime` provides. The exact signatures are owned by the
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.en.md)
and the
[Location operational query and observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.en.md)
(Korean-only).

---

## `ZLinkModule.forRoot` (configuration time)

Registers the framework root with the NestJS application once. The prerequisite for every other
entry.

```ts
@Module({
  imports: [
    ZLinkModule.forRoot({
      ...zlinkFramework()
        .addRouteMesh("play")
        .listen(5501)
        .setRoutingIdPrefix("play")
        .setPlacementWeight(100)
        .build(),
    }),
  ],
})
export class AppModule {}
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `ZLinkModule.forRoot(options: ZLinkModuleOptions)` | Required | Registers the `ZLinkModuleOptions` that `zlinkFramework()...build()` produces |
| `ZLinkModule.forRootFactory({ useFactory, inject?, imports? })` | — | An overload used to build options via an async factory |
| `zlinkFramework(): ZLinkNestFrameworkOptionsBuilder` | Required entry point | Starts the fluent builder for every registration — topology, handlers, Location Store, and more. Ends with `.build()` to produce `ZLinkModuleOptions` |

**Completion result.** Registers synchronously with no return value. It validates the
configuration at NestJS module initialization time, and if it fails, fails startup itself with a
`ZLinkConfigurationException` — a bad configuration never surfaces for the first time while
messages are already being processed.

**When to use.** Every host calls this exactly once. See the topology-discovery category for the
topology/handler registration details of `ZLinkNestFrameworkOptionsBuilder`.

---

## `relocate`

Moves the stateful objects (User Spots, Actors) the current host holds to another eligible node.
Call it before planned maintenance or a rolling update. Provided by the
`ZLinkFrameworkRuntime` injected via the `ZLINK_FRAMEWORK_RUNTIME` DI token.

```ts
const result = await frameworkRuntime.relocate({
  mode: ZLinkFrameworkRelocationMode.RollingUpdate,
  targetApplicationVersion: 2n,
  deadlineMs: 5 * 60_000,
});

if (result.outcome === ZLinkFrameworkRelocationOutcome.Relocated) {
  await frameworkRuntime.shutdown();
}
```

**Options.** The fields of `ZLinkFrameworkRelocationOptions` are as follows.

| Field | Default | Meaning |
| --- | --- | --- |
| `mode` | Required | `PlannedMaintenance` (only targets the same application version as the source) or `RollingUpdate` (only targets the specified version). Cannot be omitted |
| `targetApplicationVersion` | Must not be specified for `PlannedMaintenance`; required for `RollingUpdate` (must be greater than the source) | The target application version. If the combination is invalid, it is rejected with `TypeError` before admission changes |
| `deadlineMs` | Framework default deadline | The upper bound for waiting on eligible target convergence |
| `signal` | None | Cancels only the waiter of this `Promise`. Does not cancel a shared relocation operation that has already started |

**Completion result.** If `ZLinkFrameworkRelocationResult.outcome` is `Relocated`, every object
has finished moving and the host reaches the `Relocated` state (it accepts no new operations but
keeps infrastructure connections). If `Blocked`, `reason` carries values such as
`TargetUnavailable`/`StoreUnavailable`/`DeadlineExceeded`.

**When to use.** Use this for zero-downtime relocation before a deployment. To shut down directly
without relocating, call `shutdown` directly. A duplicate call with the same mode/target version
joins the in-flight operation; a call with different values completes with
`Blocked/OperationInProgress`.

---

## `shutdown`

Shuts the host down. It does not start a relocation — call `relocate` first if relocation is
needed.

```ts
const result = await frameworkRuntime.shutdown({ deadlineMs: 30_000 });
```

**Options.** The fields of `ZLinkFrameworkLifecycleOptions` are as follows.

| Field | Default | Meaning |
| --- | --- | --- |
| `deadlineMs` | Framework default | The upper bound for teardown. Exceeding it completes with `ForceStopped` |
| `signal` | None | Cancels only the waiter of this `Promise` |

**Completion result.** `ZLinkFrameworkTerminationResult.outcome` is `Stopped` (clean teardown) or
`ForceStopped` (deadline exceeded or teardown failure). Calling it from `Serving` cleans up
remaining application processing and resources; calling it from `Relocated` cleans up only the
infrastructure connections.

**When to use.** Always call this when shutting down the host. Calling it during `Relocating`
finalizes only the atomic relocation unit currently in progress and does not start the rest — a
caller waiting on that relocation receives `Blocked/ShutdownRequested`.

---

## `status` / `observe` (read/observe)

Reads the host's current status once, or observes status changes in real time.

```ts
const status = frameworkRuntime.status;
const canAcceptNewOperations = status.isReady && status.acceptingWork;

for await (const observed of frameworkRuntime.observe()) {
  // check observed.status, observed.loss
}
```

**Options.** `status` is a property (getter), and `observe(signal?)` returns an `AsyncIterable` —
neither takes a modifier argument (`observe` only optionally takes an `AbortSignal` for
cancellation).

**Completion result.** `status` is a synchronous read. `isReady` is `true` only when
`state === Serving`, and `acceptingWork` indicates whether new application operations are
accepted — since the two can differ, check both. `observe(...)` streams with no terminal
completion, and `ZLinkObservedStatus.loss` (`coalescedCount`/`discardedTerminalCount`) tells you
whether observations were lost. Only aborting `signal` ends that iteration.

**When to use.** Use `status` when only the status at this exact moment is needed, and
`observe(...)` to keep receiving state transitions without missing any.
`ZLinkDrainHealthIndicator` (integrating with NestJS `@nestjs/terminus`) also reads this
runtime's RouteMesh status to build a readiness probe — see the status-query entry in the
topology-discovery category.

---

See the
[NestJS host adapter exact interface](../../common/spec/server/languages/node/interfaces/07-nestjs-host.en.md)
and the
[Location operational query and observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.en.md)
(Korean-only) for the full rationale.
