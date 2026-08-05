[한국어](04-eventing.ko.md) | English

[Reference index](README.en.md)

# 04. Eventing

This category covers socket monitoring, the reusable poller, and standalone timers —
`OpenSocketMonitor(...)`, `NewPoller()`, and `NewTimer()` respectively, plus the standalone `Poll(...)`
function. `Timer`, `Poller`, and `Poll` are all declared in `poller_timer.go` — there is no separate
timer file, unlike some other languages. The exact signatures are owned by
[`internal/native/monitor.go`](../../../../bindings/go/internal/native/monitor.go) and
[`poller_timer.go`](../../../../bindings/go/internal/native/poller_timer.go), re-exported as
aliases through
[`contracts/eventing.go`](../../../../bindings/go/contracts/eventing.go).

---

## `SocketMonitor`

Observes a socket's connection lifecycle events and reads its current status.

```go
monitor, err := contracts.OpenSocketMonitor(dealer, contracts.MonitorEventConnectionReady)
monitor.OnEvent(func(event *contracts.MonitorEvent) {
    fmt.Println(event.RemoteAddr)
})
status, err := monitor.Status()
```

**Options.**

| Member | Meaning |
| --- | --- |
| `OpenSocketMonitor(socket SocketTarget, events ...MonitorEventMask) (*SocketMonitor, error)` | opens the monitor; **variadic and genuinely filtering** — passing no `events` subscribes to `MonitorEventAll`, but any masks supplied are OR-combined and actually honored, unlike rust's `SocketMonitor::open`, which accepts no such parameter and always subscribes to every event regardless of any mask value constructed |
| `Recv(flags RecvFlags) (*MonitorEvent, error)` | single entry point for both blocking and non-blocking; pass `RecvFlagsDontWait` for the non-blocking form — **the value-return-on-no-data shape here is a documented exception**, per its own doc comment: "Value-return form is allowed for monitor/timer control-plane APIs by doc/spec/bindings/go/README.md §Receive And Subscribe Shape" |
| `Status() (*MonitorStatus, error)` | returns a point-in-time snapshot |
| `OnEvent(handler func(*MonitorEvent)) error` | registers a passive lifecycle-event callback |
| `Close() error` | closes the monitor |

**Completion result.** All members are synchronous. `SocketTarget` (Core category) is the shared
interface every built-in socket type implements — the same interface `Proxy`/`Poller` registration
uses.

**When to use.** Use `OnEvent` for a passive lifecycle observer registered once; use `Recv` for a
pull-based drain loop instead. Pass specific `MonitorEventMask` values to `OpenSocketMonitor` to
limit the subscription — unlike rust, this binding's mask parameter actually has an effect.

---

## `MonitorEventMask` / `MonitorEventType`

Two distinct named types report the same sixteen lifecycle-event bits: `MonitorEventMask`
(`uint32`) is the type `OpenSocketMonitor`'s variadic parameter takes; `MonitorEventType`
(`uint64`) is the type `MonitorEvent.Event` reports. Every named constant exists in both families
with matching names and numeric values (`MonitorEventConnected` / `MonitorEventTypeConnected`,
...) — a caller comparing a subscribed mask against a received event's type must convert between
the two explicitly; they are not interchangeable without a cast, despite carrying the same bits.

**Options.**

| Member | Meaning |
| --- | --- |
| `MonitorEventMask` constants | `MonitorEventConnected`, `MonitorEventConnectDelayed`, `MonitorEventConnectRetried`, `MonitorEventListening`, `MonitorEventBindFailed`, `MonitorEventAccepted`, `MonitorEventAcceptFailed`, `MonitorEventClosed`, `MonitorEventCloseFailed`, `MonitorEventDisconnected`, `MonitorEventMonitorStopped`, `MonitorEventHandshakeFailedNoDetail`, `MonitorEventConnectionReady`, `MonitorEventHandshakeFailedProtocol`, `MonitorEventHandshakeFailedAuth`, `MonitorEventPeerWeightChanged`, `MonitorEventAll` |
| `MonitorEventType` constants | mirrors every `MonitorEventMask` value above with a `MonitorEventType`-prefixed name (`MonitorEventTypeConnected`, ...) |
| `MonitorSourceKind` (`uint32`) | `MonitorSourceSocket` — the only value |

**Completion result.** N/A — plain bitmask value types, both used purely for subscription/
comparison.

**When to use.** Pass `MonitorEventMask` constants (OR'd together, or as separate variadic
arguments) to `OpenSocketMonitor`. Compare `MonitorEvent.Event` against `MonitorEventType`
constants, not the `MonitorEventMask` family, or convert explicitly.

---

## `MonitorEvent`

A single socket connection-lifecycle event reported by a monitor.

```go
if event.IsConnected() {
    // ...
}
```

**Options.** Convenience predicate methods cover only a subset of the full lifecycle event set —
**no predicate exists for `ConnectDelayed`, `ConnectRetried`, `BindFailed`, `AcceptFailed`,
`Closed`, `CloseFailed`, `MonitorStopped`, `HandshakeFailedNoDetail`, `HandshakeFailedProtocol`,
`HandshakeFailedAuth`, or `PeerWeightChanged`** — a caller must bit-test
`event.Event&MonitorEventTypeX` directly for any of those, the same gap as every other language's
`MonitorEvent` predicate set.

| Field | Type | Meaning |
| --- | --- | --- |
| `Event` | `MonitorEventType` | the kind of lifecycle event |
| `Value` | `uint32` | an event-specific value |
| `RoutingID` | `RoutingID`, zero-value when absent | the peer routing id, present when the event provides one |
| `LocalAddr` / `RemoteAddr` | `string` | the local/remote address associated with the event |
| `HasRoutingID()` / `IsConnected()` / `IsDisconnected()` / `IsListening()` / `IsAccepted()` / `IsConnectionReady()` | `bool` | predicate for that specific event kind |

**Completion result.** N/A — an immutable value delivered by the monitor.

**When to use.** Use the named `Is*` predicates for the five lifecycle transitions they cover; for
any other event kind, bit-test `Event` against the documented `MonitorEventType` constant directly.

---

## `MonitorStatus`

A point-in-time snapshot of a monitored entity's state and auto-high-water-mark telemetry, returned
by `SocketMonitor.Status()`. A plain public-field struct.

**Options.** No parameters — every field is public.

| Group | Fields |
|---|---|
| ABI identity | `ABIVersion`, `StructSize` (`uint32`) |
| Source/state | `SourceKind` (`MonitorSourceKind`: only `MonitorSourceSocket`), `StateFlags`/`DetailFlags` (`uint32` bitmasks), `IsReady()` (computed method) |
| Pending counts | `SndPendingMsgs`, `RcvPendingMsgs` (`uint64`) |
| Auto-HWM config | `AutoHwmEnabled` (`bool`), `AutoHwmProfile`/`AutoHwmRole`/`AutoHwmPolicyClass` (`uint32`), `AutoHwmUnitBudgetBytes`/`AutoHwmSocketMessageSlots` (`uint64`), `AutoHwmSizeCap` (`uint32`) |
| Connection bucket | `AutoHwmConnectionBucketEnabled` (`bool`), `AutoHwmConnectionBucketCount`/`Index`/`Hwm4K` (`uint32`), `AutoHwmConnectionBucketHysteresisRetained` (`bool`) |
| Auto-HWM plan (bytes) | `AutoHwmEffectiveMessageBytes`, `AutoHwmPlannedSndHwmBytes`/`RcvHwmBytes`, `AutoHwmAppliedSndHwmBytes`/`RcvHwmBytes` (`uint64`), `AutoHwmEffectiveSndBuf`/`RcvBuf` (`int32`) |
| Auto-HWM recalc | `AutoHwmLastRecalcMs` (`uint64`), `AutoHwmLastRecalcReason` (`AutoHwmRecalcReason`, Core category), `AutoHwmSendBlockedRatioPPM` (`uint32`) |
| Auto-HWM deferred shrink | `AutoHwmDeferredSndHwmBytes`/`RcvHwmBytes` (`uint64`, valid only when the matching `AutoHwmDeferredSndHwmValid`/`RcvHwmValid` `bool` is true) |
| In-flight/charging | `SndBytesInFlight`, `RcvBytesInFlight`, `MinimumCoreMessageChargeBytes`, `OversizeMessageAdmissionCount`, `OversizeMessageAdmissionMaxBytes` (`uint64`) |

**Completion result.** N/A — plain public fields, plus the one computed method `IsReady()`. **No
`IsClosed()` method exists on this type in this binding**, unlike rust's `is_closed()` alongside
`is_ready()`.

**When to use.** Call `IsReady()` instead of decoding `StateFlags` directly. Use the
connection-bucket and auto-HWM-plan fields when diagnosing why a socket's effective send/receive
HWM differs from its configured `CommonSocketOptions` value (Sockets category).

---

## `Poller`

Multiplexes sockets, file descriptors, and timers on a single reusable wait.

```go
poller, err := contracts.NewPoller()
poller.AddSocket(dealer, contracts.PollIn, 1)
poller.AddTimer(timer, 2)
events := make([]contracts.PollEvent, 8)
ready, err := poller.Wait(events, time.Second)
```

**Options.**

| Member | Meaning |
| --- | --- |
| `NewPoller() (*Poller, error)` | creates the poller |
| `AddSocket(socket SocketTarget, events PollEventFlag, slot uintptr) error` | registers a socket; `slot` is a caller token echoed back in the matching result |
| `AddFd(fd int, events PollEventFlag, slot uintptr) error` | registers a raw file descriptor, same shape |
| `AddTimer(timer *Timer, slot uintptr) error` | registers a timer to be multiplexed alongside sockets/fds |
| `ModifySocket(socket, events) error` / `ModifyFd(fd, events) error` | replaces the watched events for an already-registered socket/fd; `ModifySocket` rejects a `PollCompletion` flag change specifically — that registration mode must be changed via `RemoveSocket` + `AddSocket` instead, since completion processing has separate ownership in Core |
| `RemoveSocket(socket) error` / `RemoveFd(fd int) error` / `RemoveTimer(timer *Timer) error` | unregisters the source |
| `Wait(events []PollEvent, timeout time.Duration) (int, error)` | blocks up to `timeout`, writing up to `len(events)` results in place; **takes `time.Duration`**, unlike rust's raw millisecond `i64`; treats an interrupted native wait (`EINTR`) as `(0, nil)` rather than an error |
| `Size() int` | the number of currently registered sources; returns `0` on any internal error rather than propagating one |

**Completion result.** Registration/removal members return `error`. `Wait` returns `(int, error)`
— the ready count.

**When to use.** Use one poller across a service's lifetime. Reuse one `[]PollEvent` slice across
`Wait` calls rather than allocating one per wait.

---

## `PollItem` / `PollEvent` / `Poll(...)`

`PollItem` is a raw poll descriptor used by the standalone `Poll(...)` function instead of
`Poller`; `PollEvent` is one ready source reported by `Poller.Wait`.

```go
items := []contracts.PollItem{{Socket: dealer, Events: contracts.PollIn}}
ready, err := contracts.Poll(items, 500*time.Millisecond)
if items[0].REvents&contracts.PollIn != 0 { /* ... */ }
```

**Options — `PollItem`** (all fields public).

| Field | Type | Meaning |
| --- | --- | --- |
| `Socket` | `SocketTarget`, may be `nil` for a plain fd entry | the socket this item watches |
| `Fd` | `int` | the file descriptor this item watches |
| `Events` / `REvents` | `PollEventFlag` | watched / returned poll-event bitmask |

**Options — `PollEvent`**, as returned by `Poller.Wait`. **The underlying socket/timer reference on
a `PollEvent` is unexported** — unlike `PollItem.Socket`, which is a public field, a `PollEvent`
from `Wait` exposes no way to recover the original socket or `*Timer` directly; a caller must
correlate through `Slot` back to whatever it registered.

| Field | Type | Meaning |
| --- | --- | --- |
| `SourceKind` | `PollSourceKind`: `PollSourceSocket`/`PollSourceFD`/`PollSourceTimer` | whether the source is a socket, file descriptor, or timer |
| `Fd` | `int` | the file descriptor, populated for FD-kind sources |
| `Slot` | `uintptr` | the caller token supplied at registration |
| `Revents` | `PollEventFlag` | raw poll-event bitmask |

**Options — `Poll(...)`.** `Poll(items []PollItem, timeout time.Duration) (int, error)` polls a
batch of `PollItem`s once, independent of any `Poller` instance, writing `REvents` back into each
item in place.

**Completion result.** `PollItem`/`PollEvent` are plain value types. `Poll` returns `(int, error)`
— the ready count, mutating `items` in place.

**When to use.** Branch on `PollEvent.SourceKind`/`Slot` to route each `Poller.Wait` result back to
the socket, descriptor, or timer it corresponds to. Use `PollItem`/`Poll` only for a one-shot batch
poll outside a `Poller`'s registration-based model.

---

## `Timer`

A timer that fires on an interval and can be polled or awaited, created independently of `Poller`
but registerable with one via `Poller.AddTimer`.

```go
timer, err := contracts.NewTimer()
timer.OnFire(func(t *contracts.Timer, fireCount uint64) {
    fmt.Println("fired", fireCount, "times")
})
timer.Start(1_000_000_000, 0) // interval in nanoseconds, not time.Duration
```

**Options.**

| Member | Meaning |
| --- | --- |
| `NewTimer() (*Timer, error)` | creates the timer |
| `Start(intervalNs, repeatCount uint64) error` | starts firing on `intervalNs`; **the interval is a raw nanosecond `uint64`, not `time.Duration`**, breaking from every duration-typed option elsewhere in this binding (Core/Sockets categories); `repeatCount` of `0` means repeat indefinitely |
| `Stop() error` | stops firing; restartable via `Start` |
| `Recv() (uint64, bool, error)` | the cumulative fire count; the `bool` is `false` when nothing is pending rather than an error — **the same documented value-return-on-no-data exception as `SocketMonitor.Recv`**, per its own doc comment citing doc/spec/bindings/go/README.md §Receive And Subscribe Shape |
| `OnFire(handler func(timer *Timer, fireCount uint64)) error` | registers a passive interval callback |
| `Close() error` | closes the timer |

**Completion result.** All members are synchronous.

**When to use.** Use `OnFire` for a passive interval callback; use `Recv` to poll expirations
instead, or register the timer with `Poller.AddTimer` to multiplex it alongside sockets on one
wait.

---

## Eventing constants

| Constant | Used by | Values |
|---|---|---|
| `PollEventFlag` (named `int16`) | `Poller.AddSocket`/`ModifySocket`/`AddFd`/`ModifyFd`, `PollItem.Events`/`.REvents`, `PollEvent.Revents` | `PollIn` (`1`), `PollOut` (`2`), `PollErr` (`4`), `PollPri` (`8`), `PollCompletion` (`32`) — **this binding declares all five, including `PollErr`/`PollPri`**, unlike rust, which has no equivalent to either |
| `MonitorSourceKind` (named `uint32`) | `MonitorStatus.SourceKind` | `MonitorSourceSocket` |
| `PollSourceKind` (named `int32`) | `PollEvent.SourceKind` | `PollSourceSocket`, `PollSourceFD`, `PollSourceTimer` |

---

See
[`internal/native/monitor.go`](../../../../bindings/go/internal/native/monitor.go),
[`poller_timer.go`](../../../../bindings/go/internal/native/poller_timer.go), and the
[Go binding spec](../../spec/go/README.en.md) for the full rationale.
