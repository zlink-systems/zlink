# 01. Host lifecycle

[Reference index](README.en.md)

This category covers the Spring Boot host registration entry points and the entry points
`ZLinkFrameworkRuntime` provides. The exact signatures are owned by the
[Java common runtime exact interface](../../common/spec/server/languages/java/interfaces/common-runtime.en.md)
and the
[Java configuration and host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.en.md)
(Korean-only).

---

## `@EnableZLinkFramework` + `ZLinkFrameworkConfigurer` (configuration time)

Registers the framework root with the Spring application context once. The prerequisite for
every other entry.

```java
@Configuration
@EnableZLinkFramework
public class GameFrameworkConfig {

    @Bean
    ZLinkFrameworkConfigurer gameConfigurer() {
        return options -> {
            ZLinkMeshNodeBuilder play = options.addRouteMesh("play")
                .listen(5501)
                .setRoutingIdPrefix("play")
                .setPlacementWeight(100);
        };
    }
}
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `@EnableZLinkFramework` | Required | Activates the Spring starter's auto-configuration |
| `ZLinkFrameworkConfigurer` bean (`configure(ZLinkFrameworkOptions)`) | Required | The entry point for every registration — topology, handlers, Location Store, and more. Registering several beans applies all of them in order |

**Completion result.** Registers synchronously with no return value. It validates the
configuration when the Spring context initializes (bean creation), and if it fails, fails
startup itself with a `ZLinkConfigurationException` — a bad configuration never surfaces for the
first time while messages are already being processed.

**When to use.** Every host registers this exactly once (via one or more configurer beans). See
the topology-discovery category for the topology/handler registration details of
`ZLinkFrameworkOptions`. The application does not create or start `ZLinkFrameworkRuntime`
directly — the Spring starter owns it via `SmartLifecycle.start()`.

---

## `relocate`

Moves the stateful objects (User Spots, Actors) the current host holds to another eligible node.
Call it before planned maintenance or a rolling update.

```java
ZLinkFrameworkRelocationOptions options = new ZLinkFrameworkRelocationOptions(
    ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
    2L,
    Duration.ofMinutes(5));

ZLinkFrameworkRelocationResult result = runtime.relocate(options)
    .toCompletableFuture().get();

if (result.outcome() == ZLinkFrameworkRelocationOutcome.RELOCATED) {
    runtime.shutdown().toCompletableFuture().get();
}
```

**Options.** The components of `ZLinkFrameworkRelocationOptions` are as follows.

| Component | Default | Meaning |
| --- | --- | --- |
| `mode` | Required | `PLANNED_MAINTENANCE` (only targets the same application version as the source) or `ROLLING_UPDATE` (only targets the specified version) |
| `targetApplicationVersion` | `null` for `PLANNED_MAINTENANCE` (uses the source's value); required for `ROLLING_UPDATE` (must be greater than the source) | The target application version. If the combination is invalid, it is rejected with `IllegalArgumentException` before it starts |
| `deadline` | The Framework default deadline if `null` | The upper bound for waiting on eligible target convergence |

**Completion result.** If `ZLinkFrameworkRelocationResult.outcome()` is `RELOCATED`, every object
has finished moving and the host reaches the `RELOCATED` state (it accepts no new operations but
keeps infrastructure connections). If `BLOCKED`, `reason()` carries values such as
`TARGET_UNAVAILABLE`/`STORE_UNAVAILABLE`/`DEADLINE_EXCEEDED`. Each call returns a dedicated
`CompletableFuture` view that follows the shared operation's result —
`toCompletableFuture().cancel(...)` releases only that waiter, and the host operation itself
keeps running.

**When to use.** Use this for zero-downtime relocation before a deployment. To shut down directly
without relocating, call `shutdown` directly. A duplicate call with the same mode/target version
joins the in-flight operation; a call with different values completes with
`BLOCKED/OPERATION_IN_PROGRESS`.

---

## `shutdown`

Shuts the host down. It does not start a relocation — call `relocate` first if relocation is
needed.

```java
ZLinkFrameworkTerminationResult result = runtime.shutdown(Duration.ofSeconds(30))
    .toCompletableFuture().get();
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `shutdown()` | None (overload) | Uses the Framework default deadline |
| `shutdown(Duration deadline)` | — | The upper bound for teardown. Exceeding it completes with `FORCE_STOPPED` |

**Completion result.** `ZLinkFrameworkTerminationResult.outcome()` is `STOPPED` (clean teardown)
or `FORCE_STOPPED` (deadline exceeded or teardown failure). Calling it from `SERVING` cleans up
remaining application processing and resources; calling it from `RELOCATED` cleans up only the
infrastructure connections.

**When to use.** Always call this when shutting down the host. Calling it during `RELOCATING`
finalizes only the atomic relocation unit currently in progress and does not start the rest — a
caller waiting on that relocation receives `BLOCKED/SHUTDOWN_REQUESTED`.

---

## `status` / `observe` (read/observe)

Reads the host's current status once, or observes status changes in real time.

```java
ZLinkFrameworkRuntimeStatus status = runtime.status();
boolean canAcceptNewOperations = status.isReady() && status.acceptingWork();

runtime.observe().subscribe(new Flow.Subscriber<>() {
    // check observed.status(), observed.loss() in onNext(observed)
});
```

**Options.** This entry point has no modifiers — `status()` takes no arguments, and `observe()`
returns `Flow.Publisher<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>`.

**Completion result.** `status()` is a synchronous call. `isReady()` is `true` only when
`state() == SERVING`, and `acceptingWork()` indicates whether new application operations are
accepted — since the two can differ, check both. `observe()` streams with no terminal
completion, and `ZLinkObservedStatus.loss()` (`coalescedCount`/`discardedTerminalCount`) tells
you whether observations were lost.

**When to use.** Use `status()` when only the status at this exact moment is needed, and
`observe()` to keep receiving state transitions without missing any. When implementing a Spring
Boot Actuator `HealthIndicator`, also read this `status()` to decide `Health.up()`/
`Health.outOfService()` — since the Framework does not provide a dedicated health builder, the
application's `HealthIndicator` bean writes this judgment directly.

---

See the
[Java common runtime exact interface](../../common/spec/server/languages/java/interfaces/common-runtime.en.md)
and the
[Java configuration and host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.en.md)
(Korean-only) for the full rationale.
