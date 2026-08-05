[한국어](04-eventing.ko.md) | English

[Reference index](README.en.md)

# 04. Eventing

This category covers socket monitoring, the reusable poller, and standalone timers — created via
`ISocket.MonitorOpen(...)` (Sockets category) and `Zlink.CreatePoller()`/`Zlink.CreateTimer()`
(Core category) respectively. The exact signatures are owned by
[`Contracts/Eventing/`](../../../../bindings/dotnet/src/Zlink/Contracts/Eventing/).

---

## `ISocketMonitor`

Observes a socket's connection lifecycle events and reads its current status.

```csharp
using ISocketMonitor monitor = socket.MonitorOpen(SocketEvent.Connected | SocketEvent.Disconnected);
monitor.OnEvent(e => logger.LogInformation("{Event} {Remote}", e.Event, e.RemoteAddr));
MonitorStatus status = monitor.Status();
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `OnEvent(Action<MonitorEvent> handler)` | — | registers a passive observer, invoked on a background dispatch thread for every lifecycle event |
| `Recv(RecvFlags flags)` | `RecvFlags.None` | pulls the next event; returns `MonitorEvent?`, null when `DontWait` and nothing pending |
| `Status()` | — | a point-in-time snapshot of the monitored socket's state |
| `Close()` | — | releases the monitor's native resources |

`MonitorEvent(MonitorEventType Event, uint Value, RoutingId? RoutingId, string LocalAddr, string
RemoteAddr)` is the record delivered by both `OnEvent` and `Recv` — `Value` is event-specific,
`RoutingId` present only when the event carries one.

**Completion result.** All synchronous. `ISocketMonitor` is `IDisposable`/`IAsyncDisposable`;
`Close()` releases resources without waiting on disposal semantics.

**When to use.** `OnEvent` for a passive lifecycle observer registered once; `Recv` for a
pull-based drain loop instead. `Status()` for a point-in-time snapshot.

---

## `MonitorStatus`

A snapshot of a socket's monitored state and auto-high-water-mark telemetry, returned by
`ISocketMonitor.Status()`.

**Options.** No parameters — every member below is a read-only property.

| Group | Members |
|---|---|
| ABI identity | `AbiVersion`, `StructSize` (uint) — mirrors the native `zlink_monitor_status_t` ABI version 2 |
| Source/state | `SourceKind` (`MonitorSourceKind`), `StateFlags` (`MonitorStateFlags`), `DetailFlags` (`MonitorStatusDetailFlags`), `IsReady` (computed: `SourceKind == Socket && StateFlags.Ready`) |
| Pending counts | `SndPendingMsgs`, `RcvPendingMsgs` (`ulong`) |
| Auto-HWM config | `AutoHwmEnabled` (bool), `AutoHwmProfile` (`AutoHwmProfile`), `AutoHwmRole`, `AutoHwmPolicyClass`, `AutoHwmUnitBudgetBytes`, `AutoHwmSizeCap`, `AutoHwmSocketMessageSlots` |
| Connection bucket | `AutoHwmConnectionBucketEnabled`, `AutoHwmConnectionBucketCount`, `AutoHwmConnectionBucketIndex`, `AutoHwmConnectionBucketHwm4K`, `AutoHwmConnectionBucketHysteresisRetained` |
| Auto-HWM plan (bytes) | `AutoHwmEffectiveMessageBytes`, `AutoHwmPlannedSendHighWaterMarkBytes`, `AutoHwmPlannedReceiveHighWaterMarkBytes`, `AutoHwmAppliedSendHighWaterMarkBytes`, `AutoHwmAppliedReceiveHighWaterMarkBytes`, `AutoHwmEffectiveSndbuf`, `AutoHwmEffectiveRcvbuf` |
| Auto-HWM recalc | `AutoHwmLastRecalcMs`, `AutoHwmLastRecalcReason` (`AutoHwmRecalcReason`), `AutoHwmSendBlockedRatioPpm` |
| Auto-HWM deferred shrink | `AutoHwmDeferredSendHighWaterMarkBytes`/`AutoHwmDeferredReceiveHighWaterMarkBytes` (valid only when the matching `...Valid` field is true) |
| In-flight/charging | `SendBytesInFlight`, `ReceiveBytesInFlight`, `MinimumCoreMessageChargeBytes`, `OversizeMessageAdmissionCount`, `OversizeMessageAdmissionMaxBytes` |

**Completion result.** Synchronous reads of an immutable snapshot. Every byte-valued field is
`ulong`.

**When to use.** Read `IsReady` instead of decoding `StateFlags` directly. Use the
connection-bucket and auto-HWM-plan groups when diagnosing why a socket's effective send/receive
HWM differs from its configured `CommonSocketOptions` value (Sockets category).

---

## `IPoller`

Multiplexes sockets, file descriptors, and timers on a single reusable wait.

```csharp
using IPoller poller = Zlink.CreatePoller();
poller.Add(dealer, PollEventFlags.PollIn, slot: 1);
poller.Add(timer, slot: 2);
Span<PollEvent> ready = stackalloc PollEvent[8];
int count = poller.Wait(ready, TimeSpan.FromSeconds(1));
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `Size` (`int`) | read-only | number of sources currently registered |
| `Add(IZlinkSocket, PollEventFlags, nuint slot)` | — | registers a socket for the given events; `slot` echoed back in the matching `PollEvent` |
| `AddFd(int fd, PollEventFlags, nuint slot)` | — | registers a raw file descriptor the same way |
| `Add(IZlinkTimer, nuint slot)` | — | registers a timer, ready when it fires |
| `Modify(IZlinkSocket, PollEventFlags)` / `ModifyFd(int fd, PollEventFlags)` | — | changes the watched events for an already-registered socket/descriptor |
| `Remove(IZlinkSocket)` / `Remove(IZlinkTimer)` / `Remove(int fd)` | — | unregisters the source; returns `bool`, true when it was registered |
| `Clear()` | — | unregisters every source at once |
| `Close()` | — | releases the poller's native resources |
| `Wait(Span<PollEvent> destination, TimeSpan timeout)` | — | blocks until at least one source is ready or `timeout` elapses |

**Completion result.** Registration/removal members are synchronous, non-blocking. `Wait` blocks
up to `timeout`, writing up to `destination.Length` results and returning the count written (`0`
on timeout). `IPoller` is `IDisposable`/`IAsyncDisposable`.

**When to use.** One poller across a service's lifetime. Prefer `Modify` over `Remove` + `Add`
when only the watched events change, to avoid losing the source's position.

---

## `PollEvent`

One ready source reported by an `IPoller.Wait` call.

**Options.**

| Member | Returns | Meaning |
| --- | --- | --- |
| `SourceKind` | `PollSourceKind` | `Socket`/`Fd`/`Timer` |
| `Slot` | `nuint` | the caller token supplied at registration |
| `Revents` | `PollEventFlags` | events that actually fired |
| `Fd` | `int` | populated for `Fd`-kind sources |

**Completion result.** Synchronous — a plain readonly struct, no disposal.

**When to use.** Branch on `SourceKind`/`Slot` to route each `Wait` result back to the socket,
descriptor, or timer it corresponds to.

---

## `IZlinkTimer`

A standalone timer that fires on an interval and can be awaited (via `Recv`) or driven through a
poller.

```csharp
using IZlinkTimer timer = Zlink.CreateTimer();
timer.OnFire((t, count) => logger.LogInformation("fired {Count} times", count));
timer.Start(TimeSpan.FromSeconds(1), repeatCount: 0);
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `Start(TimeSpan interval, ulong repeatCount)` | — | starts firing every `interval`; `repeatCount` bounds how many times (see source for the sentinel meaning "repeat indefinitely") |
| `Stop()` | — | stops firing; restartable via `Start` |
| `Recv(RecvFlags flags)` | `RecvFlags.None` | pulls the cumulative fire count; returns `ulong?`, null when `DontWait` and nothing pending |
| `OnFire(Action<IZlinkTimer, ulong> handler)` | — | registers a passive observer, invoked on a background dispatch thread on every fire |
| `Close()` | — | releases the timer's native resources |

**Completion result.** All synchronous. `IZlinkTimer` is `IDisposable`/`IAsyncDisposable`.

**When to use.** `OnFire` for a passive interval callback; `Recv` to poll/await expirations
instead, or register with `IPoller.Add(IZlinkTimer, nuint)` to multiplex alongside sockets.

---

## `ZlinkPoll.Poll(...)`

Static one-shot helpers that wait for readiness across several sockets or monitors at once,
without creating a reusable `IPoller`.

```csharp
int ready = ZlinkPoll.Poll(new IZlinkSocket[] { dealer, sub }, timeoutMs: 1000);
```

**Options.**

| Overload | Meaning |
| --- | --- |
| `Poll(IReadOnlyList<IZlinkSocket> sockets, int timeoutMs)` | readable check only |
| `Poll(sockets, IReadOnlyList<PollEventFlags> events, Span<PollEventFlags> revents, int timeoutMs)` | per-socket requested events, fired events written into `revents` at matching indexes |
| Same two overloads for `IReadOnlyList<ISocketMonitor>` | in place of sockets |

A negative `timeoutMs` blocks indefinitely.

**Completion result.** Synchronous; each overload returns the count of ready sources (`0` on
timeout).

**When to use.** `ZlinkPoll.Poll` for an ad hoc, one-off wait across a small fixed set; `IPoller`
instead when the watched set changes over time or timers need to be multiplexed alongside sockets.

---

## Eventing enums

Shared enums referenced across every entry above.

| Enum | Used by | Values |
|---|---|---|
| `SocketEvent` (`[Flags]`) | `ISocket.MonitorOpen(SocketEvent)` (Sockets category) | `Connected`, `ConnectDelayed`, `ConnectRetried`, `Listening`, `BindFailed`, `Accepted`, `AcceptFailed`, `Closed`, `CloseFailed`, `Disconnected`, `MonitorStopped`, `HandshakeFailedNoDetail`, `ConnectionReady`, `HandshakeFailedProtocol`, `HandshakeFailedAuth`, `PeerWeightChanged`, `All` |
| `MonitorEventType` | `MonitorEvent.Event` | Mirrors `SocketEvent`'s lifecycle values (no `All`) |
| `MonitorSourceKind` | `MonitorStatus.SourceKind` | `Socket` |
| `MonitorStateFlags` (`[Flags]`, `uint`) | `MonitorStatus.StateFlags` | `None`, `Ready`, `BoundReady`, `Closed` |
| `MonitorStatusDetailFlags` (`[Flags]`, `uint`) | `MonitorStatus.DetailFlags` | `None`, `SendPendingMessages`, `ReceivePendingMessages`, `AutoHwmBudget`, `AutoHwmBuffers` |
| `AutoHwmRecalcReason` (`uint`) | `MonitorStatus.AutoHwmLastRecalcReason` | `None`, `Initial`, `RoleChange`, `PolicyToggle`, `Refresh`, `DeferredShrink` |
| `PollSourceKind` | `PollEvent.SourceKind` | `Socket`, `Fd`, `Timer` |
| `PollEventFlags` | `IPoller.Add`/`Modify`/`Wait`, `ZlinkPoll.Poll` | `None`, `PollIn`, `PollOut`, `PollErr`, `PollPri`, `PollCompletion` |

---

See [`Contracts/Eventing/`](../../../../bindings/dotnet/src/Zlink/Contracts/Eventing/) and the
[.NET binding spec](../../spec/dotnet/README.en.md) for the full rationale.
