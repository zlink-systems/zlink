[한국어](04-eventing.ko.md) | English

[Reference index](README.en.md)

# 04. Eventing

This category covers socket monitoring, the reusable poller, and standalone timers — created via
`Socket.monitorOpen(...)` (Sockets category) and `Zlink.createPoller()`/`Zlink.createTimer()` (Core
category) respectively. The exact signatures are owned by
[`contracts/eventing/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/eventing/).

---

## `SocketMonitor`

Observes a socket's connection lifecycle events and reads its current status.

```java
try (SocketMonitor monitor = socket.monitorOpen(MonitorEventType.CONNECTED, MonitorEventType.DISCONNECTED)) {
    monitor.onEvent(event -> logger.info("{} {}", event.event(), event.remoteAddr()));
    MonitorStatus status = monitor.status();
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `onEvent(SocketMonitorHandler handler)` | registers a passive lifecycle-event callback |
| `recv()` / `recv(RecvFlags flags)` | pulls the next event; both return `MonitorEvent` directly — not `Optional`/nullable, unlike dotnet's `MonitorEvent?` |
| `status()` | returns a `MonitorStatus` point-in-time snapshot |
| `IGNORE_HANDLER` | a public static constant no-op `SocketMonitorHandler` a caller can register when it intentionally wants to discard events |

**Completion result.** All members are synchronous. `SocketMonitor extends AutoCloseable`.

**When to use.** Use `onEvent` for a passive lifecycle observer registered once; use `recv` for a
pull-based drain loop instead. Use `status()` for a point-in-time snapshot.

---

## `MonitorStatus`

A snapshot of a socket's monitored state and auto-high-water-mark telemetry, returned by
`SocketMonitor.status()`. A Java `record` — every component is immutable and accessed via its
record accessor (`status.sndPendingMsgs()`, not a `get`-prefixed method).

**Options.** No parameters — obtained via `status()`, not constructed directly.

| Group | Components |
|---|---|
| ABI identity | `abiVersion`, `structSize` (`int`) |
| Source/state | `sourceKind` (`MonitorSourceKind`), `stateFlags` (`EnumSet<MonitorStateFlags>`), `detailFlags` (`EnumSet<MonitorStatusDetailFlags>`), `isReady()` (computed method: `stateFlags.contains(MonitorStateFlags.READY)`) |
| Pending counts | `sndPendingMsgs`, `rcvPendingMsgs` (`long`) |
| Auto-HWM config | `autoHwmEnabled` (`boolean`), `autoHwmProfile` (`AutoHwmProfile`), `autoHwmRole`, `autoHwmPolicyClass` (`int`), `autoHwmUnitBudgetBytes`, `autoHwmSocketMessageSlots` (`long`), `autoHwmSizeCap` (`int`) |
| Connection bucket | `autoHwmConnectionBucketEnabled` (`boolean`), `autoHwmConnectionBucketCount`/`Index`/`Hwm4K` (`int`), `autoHwmConnectionBucketHysteresisRetained` (`boolean`) |
| Auto-HWM plan (bytes) | `autoHwmEffectiveMessageBytes`, `autoHwmPlannedSendHwmBytes`/`PlannedRecvHwmBytes`, `autoHwmAppliedSendHwmBytes`/`AppliedRecvHwmBytes` (`long`), `autoHwmAppliedSndBuffer`/`AppliedRcvBuffer` (`int`) |
| Auto-HWM recalc | `autoHwmLastRecalcMs` (`long`), `autoHwmLastRecalcReason` (`AutoHwmRecalcReason`), `autoHwmSendBlockedRatioPpm` (`int`) |
| Auto-HWM deferred shrink | `autoHwmDeferredSendHwmBytes`/`DeferredRecvHwmBytes` (`long`, valid only when the matching `autoHwmDeferredSendHwmValid`/`DeferredRecvHwmValid` `boolean` is true) |
| In-flight/charging | `sendBytesInFlight`, `recvBytesInFlight`, `minimumCoreMessageChargeBytes`, `oversizeMessageAdmissionCount`, `oversizeMessageAdmissionMaxBytes` (`long`) |

**Completion result.** N/A — an immutable record snapshot.

**When to use.** Call `isReady()` instead of decoding `stateFlags` directly. Use the
connection-bucket and auto-HWM-plan components when diagnosing why a socket's effective send/
receive HWM differs from its configured `CommonSocketOptions` value (Sockets category).

---

## `Poller`

Multiplexes sockets, file descriptors, and timers on a single reusable wait.

```java
try (Poller poller = Zlink.createPoller()) {
    poller.add(dealer, 1L, PollEventFlags.POLLIN);
    poller.add(timer, 2L);
    PollEvents events = new PollEvents(8);
    int ready = poller.wait(events, Duration.ofSeconds(1));
    for (int i = 0; i < events.readyCount(); i++) {
        PollEvent event = events.eventAt(i);
    }
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `add(Socket socket, long slot, PollEventFlags... events)` | registers a socket; `slot` is a caller token echoed back in the matching poll result — note the parameter order (`slot` before the varargs `events`) |
| `addFd(int fd, long slot, PollEventFlags... events)` | registers a raw file descriptor, same `slot`/varargs shape |
| `add(ZlinkTimer timer, long slot)` | registers a timer to be multiplexed alongside sockets/fds |
| `modify(Socket socket, PollEventFlags... events)` | replaces the watched events for an already-registered socket |
| `modifyFd(int fd, PollEventFlags... events)` | replaces the watched events for an already-registered fd |
| `remove(Socket)` / `remove(int fd)` / `remove(ZlinkTimer)` | unregisters the source; returns `boolean`, `true` when it was actually registered |
| `clear()` | unregisters every source at once |
| `size()` | `int`, the number of currently registered sources |
| `wait(PollEvents events, Duration timeout)` | blocks up to `timeout`, populating `events` in place |

**Completion result.** Registration/removal members are synchronous. `wait` blocks up to `timeout`,
populating `events` in place and returning the ready count (also readable afterward via
`events.readyCount()`).

**When to use.** Use one poller across a service's lifetime. Prefer `modify` over `remove` + `add`
when only the watched events change. Reuse one `PollEvents` buffer across `wait` calls the way
`Received` is reused for receiving, rather than allocating one per wait.

---

## `PollEvents`

A pre-allocated result buffer for a poller wait, holding up to `capacity` ready events — a
Java-specific design distinct from dotnet's `Span<PollEvent>`/cpp's raw pointer-and-capacity pair.

```java
PollEvents events = new PollEvents(16);
poller.wait(events, Duration.ofMillis(500));
for (int i = 0; i < events.readyCount(); i++) {
    if (events.hasEvent(i, PollEventFlags.POLLIN)) { /* ... */ }
}
```

**Options.** Public constructor `PollEvents(int capacity)` (throws `IllegalArgumentException` if
`capacity <= 0`). Every accessor below is bounds-checked against `readyCount()` (not `capacity()`)
and throws `IndexOutOfBoundsException` out of range.

| Member | Meaning |
| --- | --- |
| `capacity()` | the fixed capacity passed to the constructor |
| `readyCount()` | how many of the slots hold a ready event after the last `wait` |
| `sourceKind(int index)` | the ready source's kind at that index |
| `slot(int index)` | the caller token supplied when that source was registered |
| `revents(int index)` | raw `int` poll-event bitmask for that index |
| `hasEvent(int index, PollEventFlags event)` | convenience bit-test over `revents(index)` |
| `fd(int index)` | the file descriptor at that index, populated for `FD`-kind sources |
| `eventAt(int index)` | materializes a `PollEvent` record for that index |

**Completion result.** All accessors are synchronous. `Poller.wait(...)` mutates a `PollEvents`
instance in place via package-private `markReadyCount`/`markEvent` — not part of the public
contract surface.

**When to use.** Prefer `hasEvent(index, flag)` over manually bit-testing `revents(index)`. Use
`eventAt(index)` only when a caller genuinely needs the boxed `PollEvent` record — iterating the
individual accessors avoids that allocation on a hot polling loop.

---

## `PollEvent`

One ready source reported by a poller wait, materialized on demand by `PollEvents.eventAt(index)`.
A Java `record`.

**Options.** No parameters — obtained via `PollEvents.eventAt(...)`, not constructed directly.

| Component | Type | Meaning |
| --- | --- | --- |
| `sourceKind` | `PollSourceKind` | whether the source is `SOCKET`/`FD`/`TIMER` |
| `slot` | `long` | the caller token supplied at registration |
| `revents` | `int` | raw poll-event bitmask |
| `fd` | `int` | the file descriptor, populated for `FD`-kind sources |

**Completion result.** N/A — an immutable record.

**When to use.** Branch on `sourceKind()`/`slot()` to route each result back to the socket,
descriptor, or timer it corresponds to.

---

## `ZlinkTimer`

A standalone timer that fires on an interval and can be polled (via `recv`) or driven through a
poller.

```java
try (ZlinkTimer timer = Zlink.createTimer()) {
    timer.onFire((t, count) -> logger.info("fired {} times", count));
    timer.start(Duration.ofSeconds(1), 0L);
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `start(Duration interval, long repeatCount)` | starts firing on `interval`; `repeatCount == 0` means unlimited |
| `stop()` | stops firing; restartable via `start` |
| `recv()` | returns `long` directly — the cumulative fire count; unlike dotnet's `ulong?`/cpp's `std::optional<uint64_t>`, this is not nullable/optional in source |
| `onFire(TimerHandler handler)` | registers a passive interval callback; the handler receives `(ZlinkTimer timer, long fireCount)` |

**Completion result.** All members are synchronous. `ZlinkTimer extends AutoCloseable`.

**When to use.** Use `onFire` for a passive interval callback; use `recv` to poll expirations
instead, or register the timer with `Poller.add(ZlinkTimer, long)` to multiplex it alongside
sockets on one wait.

---

## Handler functional interfaces

| Interface | Registered by | Signature |
|---|---|---|
| `SocketMonitorHandler` | `SocketMonitor.onEvent(...)` | `void onEvent(MonitorEvent event)` |
| `TimerHandler` | `ZlinkTimer.onFire(...)` | `void onFire(ZlinkTimer timer, long fireCount)` |

---

## Eventing enums

Shared enums referenced across every entry above.

| Enum | Used by | Values |
|---|---|---|
| `MonitorEventType` | `Socket.monitorOpen(MonitorEventType...)` (Sockets category), `MonitorEvent.event()` | `CONNECTED`, `CONNECT_DELAYED`, `CONNECT_RETRIED`, `LISTENING`, `BIND_FAILED`, `ACCEPTED`, `ACCEPT_FAILED`, `CLOSED`, `CLOSE_FAILED`, `DISCONNECTED`, `MONITOR_STOPPED`, `HANDSHAKE_FAILED_NO_DETAIL`, `CONNECTION_READY`, `HANDSHAKE_FAILED_PROTOCOL`, `HANDSHAKE_FAILED_AUTH`, `PEER_WEIGHT_CHANGED`, `ALL` — has a static `combine(MonitorEventType...)` helper that ORs a varargs set into a raw mask |
| `MonitorSourceKind` | `MonitorStatus.sourceKind()` | `SOCKET` |
| `MonitorStateFlags` | `MonitorStatus.stateFlags()` (as `EnumSet`) | `READY`, `BOUND_READY`, `CLOSED` |
| `MonitorStatusDetailFlags` | `MonitorStatus.detailFlags()` (as `EnumSet`) | `SEND_PENDING_MESSAGES`, `RECEIVE_PENDING_MESSAGES`, `AUTO_HWM_BUDGET`, `AUTO_HWM_BUFFERS` |
| `PollSourceKind` | `PollEvent.sourceKind()`, `PollEvents.sourceKind(int)` | `SOCKET`, `FD`, `TIMER` |
| `PollEventFlags` | `Poller.add`/`modify`/`wait`, `PollEvents.hasEvent(...)` | `POLLIN`, `POLLOUT`, `POLLERR`, `POLLPRI`, `POLLCOMPLETION` — no `NONE` member (an empty state is simply zero flags supplied, not an enumerator) |

**When to use.** Java surfaces `MonitorStateFlags`/`MonitorStatusDetailFlags`/`PollEventFlags` (the
bitmask-shaped ones) as plain `enum` types consumed through `EnumSet`/varargs rather than
`[Flags]`-annotated enums (dotnet) or bitwise-OR-able flag classes (cpp) — combine them by passing
several as varargs or reading them back as an `EnumSet`, not by bitwise OR on the enum constants
themselves.

---

See [`contracts/eventing/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/eventing/)
and the [Java binding spec](../../spec/java/README.en.md) for the full rationale.
