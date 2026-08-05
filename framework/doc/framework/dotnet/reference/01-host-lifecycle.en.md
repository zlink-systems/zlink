# 01. Host lifecycle

[Reference index](README.en.md)

This category covers the `IServiceCollection` registration entry point and the entry points
`IZLinkFrameworkRuntime` provides. The exact signatures are owned by the
[Host registration exact interface](../../common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md)
and the
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.en.md)
(Korean-only).

---

## `AddZLinkFramework` (configuration time)

Registers the Framework root with the ASP.NET Core `IServiceCollection` once. It is the
prerequisite for every other entry in this reference.

```csharp
services.AddZLinkFramework(options =>
{
    var play = options.AddRouteMesh("play")
        .Listen(5501)
        .SetRoutingIdPrefix("play")
        .SetPlacementWeight(100);
});

services.AddHealthChecks()
    .AddZLinkDrainHealthCheck(); // wires host readiness into ASP.NET Core HealthCheck
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `configure: Action<IZLinkFrameworkOptions>` | required | the entry point for every topology, handler, and Location Store registration |
| `services.AddHealthChecks().AddZLinkDrainHealthCheck()` | separate registration | an `IHealthChecksBuilder` extension. Attach it to the builder `AddHealthChecks()` returns, not to `AddZLinkFramework`, to add a host readiness probe |

**Completion.** Registers synchronously with no return value. At host startup, it validates
configuration before binding the network; on failure it fails startup itself with
`ZLinkConfigurationException` — a bad configuration never surfaces for the first time while
messages are being processed.

**When to use it.** Every host calls this exactly once. For topology and handler registration
details under `IZLinkFrameworkOptions`, see the topology-discovery category.

---

## `RelocateAsync`

Moves the stateful objects (User Spot·Actor) this host currently holds to another eligible
node. Call it before planned maintenance or a rolling update.

```csharp
ZLinkFrameworkRelocationResult result = await frameworkRuntime.RelocateAsync(
    new ZLinkFrameworkRelocationOptions
    {
        Mode = ZLinkFrameworkRelocationMode.RollingUpdate,
        TargetApplicationVersion = 2,
        Deadline = TimeSpan.FromMinutes(5),
    },
    ct);

if (result.Outcome == ZLinkFrameworkRelocationOutcome.Relocated)
{
    await frameworkRuntime.ShutdownAsync(ct: ct);
}
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `Mode` | required | `PlannedMaintenance` (targets only the same version as source) or `RollingUpdate` (targets only the specified version) |
| `TargetApplicationVersion` | omitted for `PlannedMaintenance` (pinned to source), required for `RollingUpdate` | the target application version. An invalid combination completes with `ArgumentException` before the operation starts |
| `Deadline` | none (waits indefinitely) | the upper bound for waiting on eligible-target convergence |

**Completion.** When `ZLinkFrameworkRelocationResult.Outcome` is `Relocated`, every object has
finished moving and the host enters the `Relocated` state (it accepts no new operations but
keeps its infrastructure). When it is `Blocked`, `Reason` carries
`TargetUnavailable`/`StoreUnavailable`/`DeadlineExceeded`, and the host returns to `Serving` if
local objects it was still processing remain.

**When to use it.** Use it when a deployment needs a zero-downtime move. To shut down without
relocating, call `ShutdownAsync` directly. A repeated call with the same `Mode` and target
version joins the operation already in progress; a call with different values completes with
`Blocked/OperationInProgress`.

---

## `ShutdownAsync`

Shuts the host down. It does not start relocation — call `RelocateAsync` first if relocation is
needed.

```csharp
ZLinkFrameworkTerminationResult result = await frameworkRuntime.ShutdownAsync(
    deadline: TimeSpan.FromSeconds(30),
    cancellationToken: ct);
```

**Options.** The following modifier attaches to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `deadline` | 30 seconds | the upper bound for shutdown cleanup. Exceeding it completes as `ForceStopped` |

**Completion.** `ZLinkFrameworkTerminationResult.Outcome` is `Stopped` (clean cleanup) or
`ForceStopped` (deadline exceeded or cleanup failed). Calling it from `Serving` cleans up
remaining application processing and resources; calling it from `Relocated` cleans up only the
infrastructure connections. Either way, the host reaches `Stopped` once it finishes.

**When to use it.** Always call it when taking a host down. Calling it during `Relocating`
finalizes only the currently in-flight atomic relocation unit's result and does not start the
rest — callers waiting on that relocation receive `Blocked/ShutdownRequested`.

---

## `ObserveAsync` (host status stream)

Observes host status changes in real time. It delivers state transitions directly, without
polling.

```csharp
await foreach (var observed in frameworkRuntime.ObserveAsync(ct))
{
    if (observed.Status.State == ZLinkFrameworkRuntimeState.Draining)
    {
        // notify entry into graceful shutdown
    }
}
```

**Options.** This entry point has no modifiers — it only takes a `CancellationToken`.

**Completion.** It has no terminal completion; it streams
`IAsyncEnumerable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>`. `ZLinkObservedStatus.Loss`
reports how many statuses were coalesced or discarded because the consumer fell behind — that
field is the only way to tell whether an observation was lost.

**When to use it.** Use it to observe host state on a push basis. If only the current value is
needed, use the `Status` entry instead of the stream.

---

## `Status` (read)

Reads the host's current state once.

```csharp
ZLinkFrameworkRuntimeStatus status = frameworkRuntime.Status;
bool canAcceptNewOperations = status.IsReady && status.AcceptingWork;
```

**Completion.** A synchronous property. `IsReady` is true only when `State == Serving`, and
`AcceptingWork` indicates whether new application operations are accepted — the two can
diverge, so check both.

**When to use it.** Use it when only this instant's state is needed. To keep receiving state
transitions without missing any, use `ObserveAsync`.

---

The full basis is
[Host registration exact interface](../../common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md),
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.en.md), and
[Host Relocate·Shutdown·Handoff](../../common/spec/28-graceful-drain-handoff.ko.md) (all Korean-only).
