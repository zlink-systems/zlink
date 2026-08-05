[한국어](04-eventing.ko.md) | English

[Reference index](README.en.md)

# 04. Eventing

This category covers socket monitoring, the reusable poller, and poll-result buffers — created via
`Socket.monitorOpen(...)` (Sockets category) and `createPoller()` (Core category) respectively.
`Timer`/`AtomicCounter`/`Stopwatch`/`Thread` are documented in the Core category even though
`Timer`'s interface is physically declared in this category's `timer.ts` file, per the
cross-language convention this reference tree follows. The exact signatures are owned by
[`contracts/eventing/`](../../../../bindings/node/src/zlink/contracts/eventing/).

---

## `MonitorSocket`

Observes a socket's connection lifecycle events and reads its current status. (Named
`MonitorSocket` in this binding's contract, unlike other languages' `SocketMonitor`/
`socket_monitor_t`.)

```ts
const monitor = socket.monitorOpen(SOCKET_MONITOR_EVENT_ALL);
monitor.onEvent(event => logger.info(`${event.event} ${event.remoteAddr}`));
const status = monitor.status();
```

**Options.**

| Member | Meaning |
| --- | --- |
| `recv(flags?: number)` | pulls the next event; returns `MonitorEvent \| null` — `null` when nothing pending under a non-blocking flag |
| `onEvent(handler: (event: MonitorEvent) => void)` | registers a passive lifecycle-event callback |
| `status()` | returns a `MonitorStatus` point-in-time snapshot |
| `close()` | closes the monitor |

**Completion result.** All members are synchronous.

**When to use.** Use `onEvent` for a passive lifecycle observer registered once, or pass the
handler directly to `Socket.monitorOpen(events, handler)` (Sockets category) at creation time. Use
`recv` for a pull-based drain loop instead. Use `status()` for a point-in-time snapshot.

---

## `MonitorEvent`

A single socket connection-lifecycle event reported by a monitor. A `class` with a private
constructor — instances are created only by monitor `recv`/`onEvent` operations, never
directly.

**Options.** No parameters — obtained via a monitor's `recv`/`onEvent`, not constructed directly.

| Field | Type | Meaning |
| --- | --- | --- |
| `event` | `MonitorEventType` | the kind of lifecycle event |
| `value` | `number` | an event-specific value such as an error code or reconnect interval |
| `routingId` | `RoutingId \| null` | the peer routing id, present when the event provides one |
| `localAddr` / `remoteAddr` | `string` | the local/remote address associated with the event |

**Completion result.** N/A — an immutable value delivered by the monitor.

**When to use.** Branch on `event` to react to specific lifecycle transitions; read `value` for the
event-specific detail (its meaning depends on `event`).

---

## `MonitorStatus`

A snapshot of a socket's monitored state and auto-high-water-mark telemetry, returned by
`MonitorSocket.status()`. A plain read-only interface.

**Options.** No parameters — every member is a `readonly` property.

| Group | Members |
|---|---|
| ABI identity | `abiVersion`, `structSize` (`number`) |
| Source/state | `sourceKind` (`MonitorSourceKindValue`), `stateFlags`/`detailFlags` (`number` bitmasks), `isReady()` (computed method) |
| Pending counts | `sndPendingMsgs`, `rcvPendingMsgs` (`bigint`) |
| Auto-HWM config | `autoHwmEnabled` (`boolean`), `autoHwmProfile`/`autoHwmRole`/`autoHwmPolicyClass` (`number` — **not typed as `AutoHwmProfileValue`**, a raw number here unlike `ContextOptions.autoHwmProfile`), `autoHwmUnitBudgetBytes`/`autoHwmSocketMessageSlots` (`bigint`), `autoHwmSizeCap` (`number`) |
| Connection bucket | `autoHwmConnectionBucketEnabled` (`boolean`), `autoHwmConnectionBucketCount`/`Index`/`Hwm4K` (`number`), `autoHwmConnectionBucketHysteresisRetained` (`boolean`) |
| Auto-HWM plan (bytes) | `autoHwmEffectiveMessageBytes`, `autoHwmPlannedSndHwmBytes`/`PlannedRcvHwmBytes`, `autoHwmAppliedSndHwmBytes`/`AppliedRcvHwmBytes` (`bigint`), `autoHwmEffectiveSndBuf`/`EffectiveRcvBuf` (`number`) |
| Auto-HWM recalc | `autoHwmLastRecalcMs` (`bigint`), `autoHwmLastRecalcReason` (`number`), `autoHwmSendBlockedRatioPpm` (`number`) |
| Auto-HWM deferred shrink | `autoHwmDeferredSndHwmBytes`/`DeferredRcvHwmBytes` (`bigint`, valid only when the matching `autoHwmDeferredSndHwmValid`/`DeferredRcvHwmValid` `boolean` is true) |
| In-flight/charging | `sndBytesInFlight`, `rcvBytesInFlight`, `minimumCoreMessageChargeBytes`, `oversizeMessageAdmissionCount`, `oversizeMessageAdmissionMaxBytes` (`bigint`) |

**Completion result.** All properties are synchronous reads of an immutable snapshot.

**When to use.** Call `isReady()` instead of decoding `stateFlags` directly. Use the
connection-bucket and auto-HWM-plan fields when diagnosing why a socket's effective send/receive
HWM differs from its configured `CommonSocketOptions` value (Sockets category).

---

## `Poller`

Multiplexes sockets, file descriptors, and timers on a single reusable wait.

```ts
const poller = createPoller();
poller.add(dealer, [PollEventFlag.PollIn], 1);
poller.add(timer, 2);
const events = createPollEvents(8);
const ready = poller.wait(events, 1000);
```

**Options.**

| Member | Meaning |
| --- | --- |
| `size` | read-only, the number of currently registered sources |
| `add(socket: BaseSocket, events: readonly PollEventFlagValue[], slot: number)` | registers a socket; **events are supplied as a `readonly` array parameter**, not varargs (java) or a combined bitmask (dotnet/cpp) |
| `addFd(fd, events, slot)` | registers a raw file descriptor, same array/slot shape |
| `add(timer: Timer, slot: number)` | registers a timer to be multiplexed alongside sockets/fds |
| `modify(socket, events)` / `modifyFd(fd, events)` | replaces the watched events for an already-registered socket/fd |
| `remove(socket)` / `remove(timer)` / `removeFd(fd)` | unregisters the source; returns `boolean`, `true` when it was actually registered |
| `wait(events: PollEvents, timeoutMs: number)` | blocks up to `timeoutMs`, populating `events` in place; a negative timeout blocks indefinitely |

**Completion result.** Registration/removal members are synchronous. `wait` blocks up to
`timeoutMs`, filling `events` in place and returning the ready count as `number`.

**When to use.** Use one poller across a service's lifetime. Prefer `modify` over `remove` + `add`
when only the watched events change. Reuse one `PollEvents` buffer across `wait` calls rather than
creating one per wait.

---

## `PollEvents` / `PollEvent`

A reusable buffer of poll results filled by `Poller.wait(...)`, created via `createPollEvents
(capacity)` (Core category) — a design similar to java's `PollEvents`, distinct from dotnet's
`Span<PollEvent>`/cpp's raw pointer-and-capacity pair.

```ts
const events = createPollEvents(16);
poller.wait(events, 500);
for (let i = 0; i < events.readyCount; i++) {
  if (events.hasEvent(i, PollEventFlag.PollIn)) { /* ... */ }
}
```

**Options — `PollEvents`.**

| Member | Meaning |
| --- | --- |
| `capacity` | read-only `number`, the fixed capacity passed to `createPollEvents(...)` |
| `readyCount` | read-only `number`, how many of the slots hold a ready event after the last `wait` |
| `sourceKind(index)` | `number`, the ready source's kind at that index |
| `slot(index)` | `number`, the caller token supplied when that source was registered |
| `revents(index)` | `number`, raw poll-event bitmask for that index |
| `fd(index)` | `number`, the file descriptor at that index, populated for FD-kind sources |
| `hasEvent(index, event: PollEventFlagValue)` | convenience bit-test over `revents(index)`, returns `boolean` |
| `close()` | releases the buffer |

**Options — `PollEvent`.** A plain read-only interface — `sourceKind`, `slot`, `revents`, `fd` (all
`number`) — distinct from `PollEvents` and not directly produced by any entry documented in this
reference tier (unlike java's `PollEvents.eventAt(index)`, there is no equivalent materializing
method declared on this binding's `PollEvents`).

**Completion result.** All `PollEvents` accessors are synchronous. `PollEvent` is a plain read-only
value shape with no accessor methods of its own.

**When to use.** Prefer `hasEvent(index, flag)` over manually bit-testing `revents(index)`.

---

## Eventing constants

| Constant | Used by | Values |
|---|---|---|
| `MonitorSourceKind` | `MonitorStatus.sourceKind` | `Socket` — **"the Core raw API defines only the socket source"** per its own source comment |
| `MonitorEventType` | `Socket.monitorOpen(events)` (Sockets category), `MonitorEvent.event` | `Connected`, `ConnectDelayed`, `ConnectRetried`, `Listening`, `BindFailed`, `Accepted`, `AcceptFailed`, `Closed`, `CloseFailed`, `Disconnected`, `MonitorStopped`, `HandshakeFailedNoDetail`, `ConnectionReady`, `HandshakeFailedProtocol`, `HandshakeFailedAuth`, `PeerWeightChanged` — no `All` member here; use the Sockets category's `SOCKET_MONITOR_EVENT_ALL` constant instead |

---

See [`contracts/eventing/`](../../../../bindings/node/src/zlink/contracts/eventing/) and the
[Node binding spec](../../spec/node/README.en.md) for the full rationale.
