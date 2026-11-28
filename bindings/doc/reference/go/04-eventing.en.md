[한국어](04-eventing.ko.md) | English

[Reference index](README.en.md)

# 04. Eventing

This category covers socket monitoring, the reusable poller, and standalone timers —
`OpenSocketMonitor(...)`, `NewPoller()`, and `NewTimer()` respectively, plus the standalone `Poll(...)`
function. `Poller` and `Poll` are declared in `poller.go`, and `Timer` in `timer.go`. The exact signatures are owned by
[`internal/native/monitor.go`](../../../../bindings/go/internal/native/monitor.go),
[`poller.go`](../../../../bindings/go/internal/native/poller.go), and
[`timer.go`](../../../../bindings/go/internal/native/timer.go), re-exported as
aliases through
[`root_projection.go`](../../../../bindings/go/root_projection.go).

---

## `SocketMonitor`

Observes a socket's connection lifecycle events and reads its current status.

```go
monitor, err := zlink.OpenSocketMonitor(dealer, zlink.MonitorEventConnectionReady)
event, err := monitor.Recv(zlink.RecvFlagsNone)
status, err := monitor.Status()
```

**Options.**

| Member | Meaning |
| --- | --- |
| `OpenSocketMonitor(socket SocketTarget, options ...MonitorOpenOption) (*SocketMonitor, error)` | Combines event masks with OR and defaults to all events. `MonitorHwmBytes(uint64)` sets the byte HWM. |
| `Recv(flags RecvFlags) (*MonitorEvent, error)` | Pulls a monitor event. |
| `Status() (*MonitorStatus, error)` | returns a point-in-time snapshot |
| `Recv(RecvFlagsDontWait)` | Returns `nil` and a `*RecvError` with `RecvNoData` when the queue is empty. |
| `Close() error` | closes the monitor |

**Completion result.** All members are synchronous. `SocketTarget` (Core category) is the shared
interface every built-in socket type implements — the same interface `Proxy`/`Poller` registration
uses.

**When to use.** Use `Recv` for a pull-based lifecycle-event drain loop. Pass specific
`MonitorEventMask` values to `OpenSocketMonitor` to
limit the subscription.

---

## `MonitorEventMask` / `MonitorEventType`

Pass a `MonitorEventMask` (`uint32`) as a `MonitorOpenOption` to select events.
`MonitorEventType` (`uint64`) is the type of `MonitorEvent.Event`. Both types have 19
event bits with corresponding names and values. Convert explicitly to compare the two types.

**Options.**

| Member | Meaning |
| --- | --- |
| `MonitorEventMask` constants | `MonitorEventConnected`, `MonitorEventConnectDelayed`, `MonitorEventConnectRetried`, `MonitorEventListening`, `MonitorEventBindFailed`, `MonitorEventAccepted`, `MonitorEventAcceptFailed`, `MonitorEventClosed`, `MonitorEventCloseFailed`, `MonitorEventDisconnected`, `MonitorEventMonitorStopped`, `MonitorEventHandshakeFailedNoDetail`, `MonitorEventConnectionReady`, `MonitorEventHandshakeFailedProtocol`, `MonitorEventHandshakeFailedAuth`, `MonitorEventPeerWeightChanged`, `MonitorEventSendFlowPaused`, `MonitorEventSendFlowResumed`, `MonitorEventFlowStateStale`, `MonitorEventAll` |
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

`SocketMonitor.Status()` returns a point-in-time status snapshot. `ABIVersion` and `StructSize`
identify the Core layout, and `SourceKind` identifies the observed resource.

| Accessor or fields | Meaning |
| --- | --- |
| `State().Has(flag)`, `Detail().Has(flag)` | Test typed state and detail flags. `IsReady()` tests `MonitorStateReady`. |
| `SndPendingMsgs`, `RcvPendingMsgs` | Number of pending messages. |
| `SndPendingBytes`, `RcvPendingBytes` | Pending accounted bytes. |
| `AutoHwmEnabled`, `AutoHwmProfile`, `AutoHwmRole`, `AutoHwmPolicyClass` | Auto-HWM configuration diagnostics. |
| `AutoHwmPlannedSndHwmBytes`, `AutoHwmPlannedRcvHwmBytes`, `AutoHwmAppliedSndHwmBytes`, `AutoHwmAppliedRcvHwmBytes` | Planned and applied HWM in bytes. |
| `AutoHwmEffectiveSndBuf`, `AutoHwmEffectiveRcvBuf` | Effective send and receive buffer sizes. |
| `AutoHwmDeferredSndHwmBytes`, `AutoHwmDeferredRcvHwmBytes` | Deferred HWM in bytes. Each field is valid when the corresponding `AutoHwmDeferredSndHwmValid` or `AutoHwmDeferredRcvHwmValid` is `true`. |
| `AutoHwmLastRecalcMs`, `AutoHwmLastRecalcReason` | Time and reason for the last Auto-HWM recalculation. |
| `IsFlowStateDetailPopulated()` | Reports whether the receive-flow fields are populated. |
| `AutoHwmSendBlockedRatioPPM` | Fraction of initial send admission attempts blocked by HWM, in parts per million. |
| `SndBytesInFlight`, `RcvBytesInFlight`, `MinimumCoreMessageChargeBytes`, `OversizeMessageAdmissionCount`, `OversizeMessageAdmissionMaxBytes` | In-flight bytes, minimum bytes charged per message, and the count and maximum size of admitted oversize messages. |

**Completion result.** `Status()` returns a snapshot with public fields and an `error`.
`IsReady()` and the flag accessors read the snapshot synchronously.

**When to use.** Call `IsReady()` to check readiness. Compare the planned, applied,
and deferred HWM fields when diagnosing why effective send or receive HWM differs from
the value set through `CommonSocketOptions`.

---

## Connection attempt snapshots

Register `PollConnect` for any outbound raw socket. When the event is ready, call
`ConnectionAttemptsSnapshot() ([]ConnectionAttempt, error)` on the same socket. Use a single
observer per socket. A successful snapshot clears readiness for the changes it read.

Each row describes one outbound connection registration. `Endpoint` gives the address and
`State` gives the current state. Compare `IntentID`, `Revision` and `LastFailureID` only for
equality. `LastFailure` preserves the last failure even after recovery. An empty failure history
has `LastFailureID == 0`. A failed snapshot returns a `*ConfigError`.

---

## `Poller`

Multiplexes sockets, file descriptors, and timers on a single reusable wait.

```go
poller, err := zlink.NewPoller()
poller.AddSocket(dealer, zlink.PollIn, 1)
poller.AddTimer(timer, 2)
events := make([]zlink.PollEvent, 8)
ready, err := poller.Wait(events, time.Second)
```

**Options.**

| Member | Meaning |
| --- | --- |
| `NewPoller() (*Poller, error)` | creates the poller |
| `AddSocket(socket SocketTarget, events PollEventFlag, slot uintptr) error` | registers a socket; `slot` is a caller token echoed back in the matching result |
| `AddFd(fd int, events PollEventFlag, slot uintptr) error` | registers a raw file descriptor, same shape |
| `AddTimer(timer *Timer, slot uintptr) error` | registers a timer to be multiplexed alongside sockets/fds |
| `ModifySocket(socket, events) error` / `ModifyFd(fd, events) error` | Replaces the watched events. Adding or removing `PollCompletion` acquires or releases completion ownership. |
| `RemoveSocket(socket) error` / `RemoveFd(fd int) error` / `RemoveTimer(timer *Timer) error` | unregisters the source |
| `Wait(events []PollEvent, timeout time.Duration) (int, error)` | blocks up to `timeout`, writing up to `len(events)` results in place; **takes `time.Duration`**, unlike rust's raw millisecond `i64`; a failed wait is a `*ConfigError` with Core's result (`ConfigBusy`, 709, while another `Wait` runs) |
| `Size() (int, error)` | the number of currently registered sources; `ConfigBusy` (709) while a `Wait` is in progress |

**Completion result.** Registration/removal members return `error`. `Wait` returns `(int, error)`
— the ready count.

**When to use.** Use one poller across a service's lifetime. Reuse one `[]PollEvent` slice across
`Wait` calls rather than allocating one per wait.

---

## `PollItem` / `PollEvent` / `Poll(...)`

`PollItem` is a raw poll descriptor used by the standalone `Poll(...)` function instead of
`Poller`; `PollEvent` is one ready source reported by `Poller.Wait`.

```go
items := []zlink.PollItem{{Socket: dealer, Events: zlink.PollIn}}
ready, err := zlink.Poll(items, 500*time.Millisecond)
if items[0].REvents&zlink.PollIn != 0 { /* ... */ }
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
timer, err := zlink.NewTimer()
timer.Start(1_000_000_000, 0) // interval in nanoseconds, not time.Duration
fireCount, ok, err := timer.Recv()
```

**Options.**

| Member | Meaning |
| --- | --- |
| `NewTimer() (*Timer, error)` | creates the timer |
| `Start(intervalNs, repeatCount uint64) error` | starts firing on `intervalNs`; **the interval is a raw nanosecond `uint64`, not `time.Duration`**, breaking from every duration-typed option elsewhere in this binding (Core/Sockets categories); `repeatCount` of `0` means repeat indefinitely |
| `Stop() error` | stops firing; restartable via `Start` |
| `Recv() (uint64, bool, error)` | the cumulative fire count; the `bool` is `false` when nothing is pending rather than an error — **the same documented value-return-on-no-data exception as `SocketMonitor.Recv`**, per its own doc comment citing doc/spec/bindings/go/README.md §Receive And Subscribe Shape |
| repeated `Recv()` | drains each pending cumulative fire count through the pull surface |
| `Close() error` | closes the timer |

**Completion result.** All members are synchronous.

**When to use.** Use `Recv` to pull expirations, or register the timer with `Poller.AddTimer` to
multiplex it alongside sockets on one
wait.

---

## Eventing constants

| Constant | Used by | Values |
|---|---|---|
| `PollEventFlag` (`int16`) | `Poller.AddSocket`/`ModifySocket`/`AddFd`/`ModifyFd`, `PollItem.Events`/`REvents`, `PollEvent.Revents` | `PollIn` (1), `PollOut` (2), `PollErr` (4), `PollPri` (8), `PollCompletion` (32), `PollRoute` (64), `PollConnect` (128). |
| `MonitorSourceKind` (named `uint32`) | `MonitorStatus.SourceKind` | `MonitorSourceSocket` |
| `PollSourceKind` (named `int32`) | `PollEvent.SourceKind` | `PollSourceSocket`, `PollSourceFD`, `PollSourceTimer` |

---

See
[`internal/native/monitor.go`](../../../../bindings/go/internal/native/monitor.go),
[`poller.go`](../../../../bindings/go/internal/native/poller.go),
[`timer.go`](../../../../bindings/go/internal/native/timer.go), and the
[Go binding spec](../../spec/go/README.en.md) for the full rationale.
