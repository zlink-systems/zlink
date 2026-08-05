[한국어](01-core.ko.md) | English

[Reference index](README.en.md)

# 01. Core

This category covers the context lifecycle, context options, routing identity, and package-level
utility functions. **Socket creation is a method on `Context`** (`ctx.PairSocket()`, ...) rather
than a top-level factory — the only wrapper binding covered so far where this is the case. The
exact signatures are owned by
[`internal/native/context.go`](../../../../bindings/go/internal/native/context.go) and
[`utility.go`](../../../../bindings/go/internal/native/utility.go), re-exported as aliases through
[`contracts/core.go`](../../../../bindings/go/contracts/core.go).

---

## `NewContext()`

Creates a messaging context — the factory and owner of sockets.

```go
ctx, err := contracts.NewContext()
defer ctx.Close()
```

**Options.** No parameters.

**Completion result.** Returns `(*Context, error)`. The caller owns it and must call `Close()` —
closing terminates anything still open under it, including sockets created from it.

**When to use.** Call once per context the application needs; most applications need exactly one.

---

## `Context.Shutdown()` / `Context.RecalculateAutoHwm()`

Interrupts blocking operations on the context's sockets without closing them, or forces an
immediate recalculation of automatic high-water marks.

```go
ctx.Shutdown()
ctx.RecalculateAutoHwm()
```

**Options.** Neither takes parameters.

**Completion result.** Both return `error`. `Shutdown` interrupts blocking calls on sockets under
this context but does not close the context or its sockets. `RecalculateAutoHwm` recomputes
automatic HWM only for sockets still configured with an `AutoHwmProfile`.

**When to use.** Call `Shutdown()` before `Close()` on a context with sockets in use across
multiple goroutines. Call `RecalculateAutoHwm()` after changing the auto-HWM profile or a
message-unit option, to apply new sizing immediately.

---

## `Context.Options()`

Returns the context-wide options facade, whose paired getter/setter methods govern I/O threads and
the defaults every socket created from the context inherits.

```go
opts := ctx.Options()
opts.SetIOThreads(8)
opts.SetAutoHwmProfile(contracts.AutoHwmProfileLowLatency)
opts.AddThreadAffinity(2)
```

**Options.** Every getter returns `(T, error)`; every setter returns `error` — unlike languages
where only a subset of accessors can fail.

| Member | Meaning |
| --- | --- |
| `IOThreads()` / `SetIOThreads(int)` | I/O thread count |
| `MaxSockets()` / `SetMaxSockets(int)` | context-wide socket cap |
| `SocketLimit()` | read-only, build's hard cap on `MaxSockets` |
| `ThreadPriority()` / `SetThreadPriority(int)` | dispatch thread priority |
| `ThreadSchedulingPolicy()` / `SetThreadSchedulingPolicy(int)` | dispatch thread scheduling policy |
| `ThreadNamePrefix()` / `SetThreadNamePrefix(string)` | OS-visible dispatch thread name prefix |
| `AutoHwmEnabled()` / `SetAutoHwmEnabled(bool)` | whether auto-HWM sizing is active |
| `AutoHwmRecalcDebounce()` / `SetAutoHwmRecalcDebounce(time.Duration)` | minimum interval between automatic recalculations |
| `MaxMessageSize()` / `SetMaxMessageSize(int)` | per-message size cap |
| `MessageStructSize()` | read-only, native message struct size, diagnostic only |
| `Blocky()` / `SetBlocky(bool)` | whether blocking calls actually block vs. fail fast |
| `AutoHwmProfile()` / `SetAutoHwmProfile(AutoHwmProfile)` | automatic HWM sizing profile — see Sockets category |
| `AutoHwmMsgUnitBytes()` / `SetAutoHwmMsgUnitBytes(int)` | accounted-byte unit for auto-HWM; `0` selects the socket-type default |
| `AddThreadAffinity(cpu int)` | setter-only, pins an I/O thread to a CPU |
| `RemoveThreadAffinity(cpu int)` | setter-only, unpins an I/O thread from a CPU |

**Completion result.** Every accessor is synchronous, returning `error` alongside its value.

**When to use.** Adjust before creating sockets when the defaults don't fit the deployment. Pair an
`AutoHwmProfile`/`AutoHwmEnabled` change with `Context.RecalculateAutoHwm()` to apply it
immediately.

---

## `Context.PairSocket()` / `DealerSocket()` / `RouterSocket()` / `PubSocket()` / `SubSocket()` / `XPubSocket()` / `XSubSocket()` / `StreamSocket()`

Creates a socket of the given type, owned by the caller. **Declared as methods on `*Context`**,
not free functions — the only wrapper binding covered so far with this shape.

```go
dealer, err := ctx.DealerSocket()
defer dealer.Close()
```

**Options.** None of the eight factory methods takes parameters.

**Completion result.** Each returns `(*SocketType, error)`. The caller owns and must `Close()` the
returned socket independently of the context.

**When to use.** See the Sockets category for each socket type's operations, options, and
capability roles — this entry only covers how each is created.

---

## `RoutingID`

A binary-safe value type identifying a messaging peer or route, 1 to 255 bytes. A value type
(`struct`, not a pointer), unlike every other language's reference/handle-shaped routing id.

```go
fromString := contracts.NewRoutingIDString("worker-3")
fromBytes := contracts.NewRoutingID(rawBytes)
fromUint32 := contracts.NewRoutingIDUint32(42)
restored, err := contracts.NewRoutingIDFromHex(previouslyPrinted.Hex())
```

**Options.** Package-level constructors; none return an error except `NewRoutingIDFromHex` — the
others panic on invalid length.

| Member | Meaning |
| --- | --- |
| `NewRoutingID(data []byte)` | copies the full slice as-is; panics out of range |
| `NewRoutingIDString(value string)` | UTF-8 encode; panics out of range |
| `NewRoutingIDUint32(value uint32)` | 4-byte big-endian; panics out of range |
| `NewRoutingIDUUIDBytes(value [16]byte)` | 16-byte, e.g. UUID bytes; panics out of range |
| `NewRoutingIDFromHex(value string) (RoutingID, error)` | restores bytes `Hex()` printed — the only constructor returning an error rather than panicking |
| `Bytes()` | defensive copy of the bytes |
| `Size()` | byte length, 1-255 |
| `Hash() uint64` | a diagnostic/map-key helper, distinct from Go's built-in `==` comparability since `RoutingID` is a fixed-size value struct |
| `Hex()` | hex encoding, round-trippable with `NewRoutingIDFromHex` |
| `Equal(other RoutingID) bool` | value equality |
| `String()` | display form: printable UTF-8, then 4-byte-as-uint32, then 16-byte-as-UUID-format, then a `hex:`-prefixed fallback |

**Completion result.** All members are synchronous. `RoutingID` being a plain value struct means it
is directly comparable with `==` and usable as a map key without `Hash()`/`Equal()`, though both are
provided as explicit alternatives.

**When to use.** Use `NewRoutingIDFromHex` (not `NewRoutingID` on hex-decoded bytes) specifically
when the input might be malformed, since it is the only routing-id constructor that reports an
error instead of panicking on invalid input.

---

## `Has(capability)`

Checks for an optional build capability.

```go
hasTLS, err := contracts.Has("tls")
```

**Options.** `capability string` — recognized names include `"tcp"`, `"ipc"`, `"tls"`, `"ws"`,
`"wss"`; any other string returns `false`.

**Completion result.** Returns `(bool, error)` — **unlike every other language's `has`/`Has`, which
never fails**, this one has an error return.

**When to use.** Use at startup to branch on optional transports rather than assuming every one is
compiled in. There is no `Strerror`/`strerror`-equivalent function in this binding — every typed
error (Errors category) formats its own message via its `Error() string` method instead of routing
through a shared native-errno-to-text lookup.

---

## `NewStopwatch()` / `NewAtomicCounter()` / `NewThread(target)`

Creates a high-resolution stopwatch, a thread-safe integer counter, or a running background
thread — three independent utility resources, all declared in `utility.go`.

```go
watch := contracts.NewStopwatch()
partialUs := watch.Intermediate()
totalUs := watch.Stop()

counter := contracts.NewAtomicCounter()
newValue := counter.Increment()

thread, err := contracts.NewThread(doWork)
_ = thread.Join()
```

**Options.**

| Member | Meaning |
| --- | --- |
| `NewStopwatch()` | returns the resource directly with no error — **no error path**, unlike most other constructors in this binding |
| `Stopwatch.Intermediate()` / `Stop()` | both `uint64` microseconds since construction, no error return; `Intermediate()` callable any number of times, `Stop()` called exactly once to finish |
| `NewAtomicCounter()` | returns the resource directly with no error, same as `NewStopwatch()` |
| `AtomicCounter.Set(int)` | assigns the counter's value, no error return |
| `AtomicCounter.Increment()` / `Decrement()` | adjusts the counter by one, returning the *new* value |
| `AtomicCounter.Value()` | reads the current value |
| `AtomicCounter.Close()` | releases the counter, no error return |
| `NewThread(target func()) (*Thread, error)` | runs `target` immediately on the new OS thread |
| `Thread.Join() error` | blocks until the task finishes |
| `Thread.Close() error` | releases the thread handle |

**Completion result.** `Stopwatch`/`AtomicCounter` construction and most of their methods have no
error path at all; `Thread` construction and its methods do.

**When to use.** Use `AtomicCounter` for a shared count safe across goroutines. Use `Stopwatch` for
benchmarking — call `Intermediate()` any number of times, then `Stop()` exactly once. Use
`NewThread` for a portable background OS thread instead of a goroutine, when the zlink runtime
needs to own the underlying native thread's lifecycle.

---

## `Proxy(...)` / `ProxySteerable(...)` / `Sleep(seconds)` / `MultipartClose(parts)`

Runs a bidirectional message-forwarding loop between two sockets (optionally steerable via a
control socket), sleeps the calling goroutine, or closes every message in a multipart slice.

```go
contracts.Proxy(frontend, backend, capture) // capture may be nil; blocks until context termination
contracts.ProxySteerable(frontend, backend, capture, control)
contracts.Sleep(1) // seconds, not milliseconds
contracts.MultipartClose(parts)
```

**Options.** Both `Proxy` and every socket type satisfy the `SocketTarget` interface (Eventing
category also uses it for `Poller`/`SocketMonitor` registration), so any concrete socket can be
passed directly.

| Member | Meaning |
| --- | --- |
| `Proxy(frontend, backend, capture SocketTarget) error` | `capture` may be `nil` |
| `ProxySteerable(frontend, backend, capture, control SocketTarget) error` | adds a required `control` source |
| `Sleep(seconds int)` | blocks the calling goroutine; no return value at all — not even the value-less `error` most other functions here return |
| `MultipartClose(parts []*Message)` | closes every part in one call |

**Completion result.** `Proxy`/`ProxySteerable` return `error` and block the calling goroutine
until the context is terminated (or, for `ProxySteerable`, until a control command or error ends
the loop) — run either on a dedicated goroutine. `Sleep`/`MultipartClose` have no return value.

**When to use.** Use `Proxy` for a simple fire-and-forget forwarding loop. Use `ProxySteerable` when
the application needs to pause/resume/terminate the loop from another goroutine via the control
socket. Use `MultipartClose` to release every message in a received or constructed multipart slice
in one call instead of a hand-written loop.

---

See [`internal/native/context.go`](../../../../bindings/go/internal/native/context.go),
[`utility.go`](../../../../bindings/go/internal/native/utility.go), and the
[Go binding spec](../../spec/go/README.en.md) for the full rationale.
