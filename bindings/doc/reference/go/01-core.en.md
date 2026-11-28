[한국어](01-core.ko.md) | English

[Reference index](README.en.md)

# 01. Core

This category covers the context lifecycle, context options, routing identity, and package-level
utility functions. **Socket creation is a method on `Context`** (`ctx.PairSocket()`, ...) rather
than a top-level factory — the only wrapper binding covered so far where this is the case. The
exact signatures are owned by
[`internal/native/context.go`](../../../../bindings/go/internal/native/context.go) and
[`utility.go`](../../../../bindings/go/internal/native/utility.go), re-exported as aliases through
[`root_projection.go`](../../../../bindings/go/root_projection.go).

---

## `NewContext()`

Creates a messaging context — the factory and owner of sockets.

```go
ctx, err := zlink.NewContext()
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
multiple goroutines. Call `RecalculateAutoHwm()` after changing the Auto-HWM profile
to apply new sizing immediately.

---

## `Context.Options()`

Returns the context-wide options facade, whose paired getter/setter methods govern I/O threads and
the defaults every socket created from the context inherits.

```go
opts := ctx.Options()
opts.SetIOThreads(8)
opts.SetCoreHwmProfile(zlink.AutoHwmProfileLowLatency)
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
| `MessageStructSize()` | read-only, native message struct size, diagnostic only |
| `Blocky()` / `SetBlocky(bool)` | whether blocking calls actually block vs. fail fast |
| `CoreHwmProfile()` / `SetCoreHwmProfile(AutoHwmProfile)` | automatic HWM sizing profile — see Sockets category |
| `CoreHwmMemoryLimitBytes()` / `SetCoreHwmMemoryLimitBytes(uint64)` | Configured memory limit in bytes. |
| `CoreHwmBudgetBytes()` / `SetCoreHwmBudgetBytes(uint64)` | Configured Core budget in bytes. |
| `AddThreadAffinity(cpu int)` | setter-only, pins an I/O thread to a CPU |
| `RemoveThreadAffinity(cpu int)` | setter-only, unpins an I/O thread from a CPU |

**Completion result.** Every accessor is synchronous, returning `error` alongside its value.

**When to use.** Adjust before creating sockets when the defaults don't fit the deployment. Pair an
`CoreHwmProfile`/`AutoHwmEnabled` change with `Context.RecalculateAutoHwm()` to apply it
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

`RoutingID` is a comparable value that stores 1–255 bytes. It can be used as a Go map key.

```go
fromString := zlink.NewRoutingIDString("worker-3")
fromUint32 := zlink.NewRoutingIDUint32(42)
fromBytes := zlink.NewRoutingID(rawBytes)
restored, err := zlink.TryNewRoutingIDFromHex(previouslyPrinted.Hex())
```

| Member | Meaning |
| --- | --- |
| `NewRoutingID(data []byte) RoutingID` / `NewRoutingIDString(value string) RoutingID` | Copy the input bytes. Lengths outside 1–255 yield the zero value. |
| `NewRoutingIDFromHex(value string) RoutingID` | Decode the output of `Hex()`. Invalid input yields the zero value. |
| `TryNewRoutingID(data []byte) (RoutingID, error)` / `TryNewRoutingIDFromHex(value string) (RoutingID, error)` | Report invalid input immediately as a `*ConfigError` with `ConfigInvalidArgument`. |
| `NewRoutingIDUint32(value uint32) RoutingID` / `Uint32() (uint32, bool)` | Write or read four big-endian bytes. |
| `NewRoutingIDUUIDBytes(value [16]byte) RoutingID` / `UUIDBytes() ([16]byte, bool)` | Write or read the 16 UUID bytes. |
| `Bytes()` / `Size()` / `Hex()` | Copy the bytes, report their length, or encode them as hex. |
| `Equal(other RoutingID) bool` / `Hash() uint64` | Compare or hash the bytes. |
| `String()` | Display form, in order of preference: printable UTF-8, a decimal uint32 for four bytes, UUID notation for 16 bytes, or `hex:` followed by hex digits. |

`Uint32()` and `UUIDBytes()` return `ok=false` when the byte length differs from the expected length.

**Completion result.** All members are synchronous. Compare IDs with `==` or `Equal()`.
A `RoutingID` can be used directly as a map key. `Hash()` returns an explicit hash.

**When to use.** Save an ID with `Hex()`. Use `TryNewRoutingIDFromHex` to report malformed
hex input and `TryNewRoutingID` to report an invalid byte slice.

---

## `Has(capability)`

Checks for an optional build capability.

```go
hasTLS, err := zlink.Has("tls")
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
watch := zlink.NewStopwatch()
partialUs := watch.Intermediate()
totalUs := watch.Stop()

counter := zlink.NewAtomicCounter()
newValue := counter.Increment()

thread, err := zlink.NewThread(doWork)
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

## `Proxy(...)` / `Sleep(seconds)` / `MultipartClose(parts)`

`Proxy` forwards messages in both directions and may copy traffic to a capture socket.

```go
err := zlink.Proxy(frontend, backend, capture) // capture may be nil
zlink.Sleep(1) // seconds
zlink.MultipartClose(parts)
```

**Options.** The eight socket types implement `SocketTarget` and can be passed directly
to `Proxy`.

| Member | Meaning |
| --- | --- |
| `Proxy(frontend, backend, capture SocketTarget) error` | Pass `nil` to omit capture. |
| `Sleep(seconds int)` | Blocks the calling goroutine for the given number of seconds. |
| `MultipartClose(parts []*Message)` | Closes each message in the slice. |

**Completion result.** `Proxy` blocks until the context terminates or forwarding fails
and returns `error`. `Sleep` and `MultipartClose` return no value.

**When to use.** Run `Proxy` in a dedicated goroutine when the caller must continue
while forwarding runs. Use `MultipartClose` to release every message in a received
or constructed multipart slice.

---

See [`internal/native/context.go`](../../../../bindings/go/internal/native/context.go),
[`utility.go`](../../../../bindings/go/internal/native/utility.go), and the
[Go binding spec](../../spec/go/README.en.md) for the full rationale.
