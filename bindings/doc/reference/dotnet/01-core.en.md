[한국어](01-core.ko.md) | English

[Reference index](README.en.md)

# 01. Core

This category covers the context lifecycle, context options, routing identity, and the `Zlink`
static facade — the library's process-wide entry points and utility resources. Socket creation
methods on `IContext` are listed here for completeness but detailed under the Sockets category;
poller/timer creation on `Zlink` is listed here but detailed under the Eventing category. The
exact signatures are owned by
[`Contracts/Core/`](../../../../bindings/dotnet/src/Zlink/Contracts/Core/).

---

## `Zlink.CreateContext()`

Creates a messaging context — the factory and owner of sockets, and the prerequisite for every
other entry in this reference.

```csharp
using IContext context = Zlink.CreateContext();
```

**Options.** No parameters.

**Completion result.** Returns `IContext` synchronously. The caller owns it and must dispose it
(`IDisposable`/`IAsyncDisposable`); disposing it terminates any sockets still open under it.

**When to use.** Once per context the application needs — most applications need exactly one.

---

## `IContext.Shutdown()` / `IContext.RecalculateAutoHwm()`

Interrupts blocking operations on the context's sockets without disposing them, or forces an
immediate recalculation of automatic high-water marks.

```csharp
context.Shutdown();
context.RecalculateAutoHwm();
```

**Options.** Neither takes parameters.

**Completion result.** Both synchronous, `void`. `Shutdown` interrupts blocking calls on sockets
under this context but doesn't dispose the context or its sockets. `RecalculateAutoHwm` recomputes
automatic HWM only for sockets still configured with an `AutoHwmProfile`.

**When to use.** Call `Shutdown` before disposing a context with sockets in use across multiple
threads, to avoid a thread blocking indefinitely. Pair `RecalculateAutoHwm` with an `AutoHwmProfile`
change to apply new sizing immediately instead of waiting for the normal refresh path.

---

## `IContext.Options`

The context-wide options facade, governing I/O threads and the defaults every socket created from
the context inherits.

```csharp
context.Options.IoThreads = 8;
context.Options.AutoHwmProfile = AutoHwmProfile.LowLatency;
context.Options.AddThreadAffinityCpu(2);
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `IoThreads` | 1 | dispatch thread count |
| `MaxSockets` | 1023 | context-wide socket cap |
| `SocketLimit` | read-only | build's hard cap on `MaxSockets` |
| `ThreadPriority` | OS default | dispatch thread priority |
| `ThreadSchedulingPolicy` | OS default | dispatch thread scheduling policy |
| `MaxMessageSize` | unbounded | per-message size cap |
| `MessageThreadSize` | read-only | native message struct size, diagnostic only |
| `Blocky` | `true` | whether blocking calls actually block vs. fail fast |
| `AutoHwmProfile` | `Balanced` | automatic HWM sizing profile — see Sockets category |
| `AutoHwmMessageUnitBytes` | profile default | accounted-byte unit for auto-HWM (`ulong`) |
| `AutoHwmEnabled` | `true` | whether auto-HWM sizing is active |
| `AutoHwmRecalcDebounce` | profile default | minimum interval between automatic recalculations |
| `ThreadNamePrefix` | none | OS-visible dispatch thread name prefix |
| `AddThreadAffinityCpu(cpu)` / `RemoveThreadAffinityCpu(cpu)` | none | pin/unpin dispatch threads to specific CPUs |

**Completion result.** Every get/set and both affinity methods are synchronous.

**When to use.** Adjust before creating sockets when the defaults don't fit the deployment.
`AutoHwmProfile`/`AutoHwmEnabled` can change on a live context — pair the change with
`RecalculateAutoHwm` above to apply it immediately.

---

## `IContext.CreatePairSocket()` / `CreateDealerSocket()` / `CreateRouterSocket()` / `CreatePubSocket()` / `CreateSubSocket()` / `CreateXPubSocket()` / `CreateXSubSocket()` / `CreateStreamSocket()`

Creates a socket of the given type, owned by the caller.

```csharp
using IDealerSocket dealer = context.CreateDealerSocket();
```

**Options.** None of the eight factory methods takes parameters — each returns its matching
interface (`IPairSocket`, `IDealerSocket`, `IRouterSocket`, `IPubSocket`, `ISubSocket`,
`IXPubSocket`, `IXSubSocket`, `IStreamSocket`).

**Completion result.** Synchronous. The caller owns and must dispose the returned socket
independently of the context.

**When to use.** See the Sockets category for each interface's operations and options — this
entry only covers creation.

---

## `RoutingId`

A binary-safe value type identifying a messaging peer or route, 1 to 255 bytes.

```csharp
RoutingId fromString = RoutingId.From("worker-3");
RoutingId fromBytes = RoutingId.From(rawBytes);
RoutingId fromUint = RoutingId.From(42u);
RoutingId fromGuid = RoutingId.From(Guid.NewGuid());
RoutingId restored = RoutingId.FromHex(previouslyPrinted.ToHex());
```

**Options.**

| Member | Default | Meaning |
| --- | --- | --- |
| `From(ReadOnlySpan<byte>)` / `From(byte[])` | — | copy raw bytes as-is |
| `From(string)` | — | UTF-8 encode |
| `From(uint)` | — | 4-byte big-endian |
| `From(Guid)` | — | 16-byte big-endian |
| `FromHex(string)` | — | restore bytes `ToHex()` printed |
| `Size` / `IsEmpty` | — | length / zero-length check |
| `ToBytes()` | — | internal-storage-backed view |
| `ToHex()` | — | round-trippable with `FromHex` |
| `TryToUInt32(out uint)` / `TryToGuid(out Guid)` | — | typed decode, `false` on shape mismatch |
| `ToString()` | — | display only: printable UTF-8, then `uint`, then `Guid`, then `hex:`-prefixed fallback |
| `Equals`/`==`/`!=`/`GetHashCode` | — | value equality |

**Completion result.** All synchronous. An out-of-range byte length (not 1..255) throws
`ArgumentOutOfRangeException`; a malformed hex string to `FromHex` throws `ArgumentException`.

**When to use.** `From(string)` for a human-assigned identity, `From(uint)`/`From(Guid)` for a
numeric or GUID identity, raw `From(byte[])` when the identity is already binary. Use
`ToHex()`/`FromHex()` for a durable round trip — `ToString()` is display-only.

---

## `Zlink.Version()` / `Zlink.Strerror(int)` / `Zlink.Has(string)`

Reads the native library's build version, converts a native error code to a message, or checks
for an optional build capability.

```csharp
var (major, minor, patch) = Zlink.Version();
string message = Zlink.Strerror(errnum);
bool hasTls = Zlink.Has("tls");
```

**Options.**

| Member | Parameters | Meaning |
| --- | --- | --- |
| `Version()` | none | linked native library version |
| `Strerror(errnum)` | `int` error code | native errno text |
| `Has(capability)` | `"tcp"`/`"ipc"`/`"tls"`/`"ws"`/`"wss"`; any other string returns `false` | optional build capability check |

**Completion result.** All synchronous. `Version()` returns `(int Major, int Minor, int Patch)`;
`Strerror` returns `string`; `Has` returns `bool`.

**When to use.** `Version()` to confirm a dynamically-loaded native library matches expectations.
`Has(...)` at startup to branch on optional transports. `Strerror` for diagnostics alongside a
native error code surfaced elsewhere.

---

## `Zlink.CreateAtomicCounter()` / `Zlink.CreateStopwatch()` / `Zlink.CreateThread(Action)`

Creates a thread-safe integer counter, a high-resolution stopwatch, or a running background
thread.

```csharp
using IAtomicCounter counter = Zlink.CreateAtomicCounter();
int newValue = counter.Increment();

using IZlinkStopwatch watch = Zlink.CreateStopwatch();
ulong partialUs = watch.Intermediate();
ulong totalUs = watch.Stop();

using IZlinkThread thread = Zlink.CreateThread(() => DoWork());
thread.Join();
```

**Options.**

| Member | Returns | Meaning |
| --- | --- | --- |
| `CreateAtomicCounter()` | `IAtomicCounter` | no parameters |
| `IAtomicCounter.Value` | `int` | get |
| `IAtomicCounter.Set(value)` / `.Increment()` / `.Decrement()` | `int` | the latter two return the *new* value, not the prior one |
| `CreateStopwatch()` | `IZlinkStopwatch` | no parameters |
| `IZlinkStopwatch.Intermediate()` / `.Stop()` | `ulong` microseconds | call `Intermediate()` any number of times, `Stop()` once |
| `CreateThread(Action task)` | `IZlinkThread` | `task` runs immediately on the new thread |
| `IZlinkThread.Join()` | — | blocks until the task finishes; repeated calls are no-ops |
| `IZlinkThread.Close()` | — | joins first if still running, then releases the handle |

**Completion result.** All three factories return their resource interface synchronously; the
caller owns and must dispose each (`IDisposable`/`IAsyncDisposable`).

**When to use.** `CreateAtomicCounter` for a shared count safe across threads. `CreateStopwatch`
for benchmarking. `CreateThread` for a portable background thread instead of a platform-specific
API.

---

## `Zlink.Proxy(...)` / `Zlink.ProxySteerable(...)` / `Zlink.Sleep(TimeSpan)` / `Zlink.MultipartClose(...)`

Runs a bidirectional message-forwarding loop between two sockets (optionally steerable via a
control socket), sleeps the calling thread, or disposes every message in a multipart payload.

```csharp
Zlink.Proxy(frontend, backend, capture); // capture may be null; blocks until context termination
Zlink.ProxySteerable(frontend, backend, capture, control); // control accepts runtime commands
Zlink.Sleep(TimeSpan.FromSeconds(1));
Zlink.MultipartClose(parts);
```

**Options.**

| Member | Parameters | Meaning |
| --- | --- | --- |
| `Proxy(frontend, backend, capture)` | `IZlinkSocket` frontend/backend (required), capture (optional) | forwards until context termination |
| `ProxySteerable(frontend, backend, capture, control)` | adds required `control` socket | pausable/resumable via `control` |
| `Sleep(TimeSpan)` | duration | blocks the calling thread |
| `MultipartClose(IReadOnlyList<Message>)` | parts to release | closes every message in one call |

**Completion result.** All four synchronous, no return value. `Proxy`/`ProxySteerable` block the
calling thread until the context terminates (or, for `ProxySteerable`, until a `TERMINATE` command
or error ends the loop) — run either on a dedicated thread.

**When to use.** `Proxy` for a simple fire-and-forget forwarding loop. `ProxySteerable` when the
loop needs to be paused/resumed/terminated from another thread via `control`. `MultipartClose` to
release a received or constructed multipart array in one call.

---

## `Zlink.CreatePoller()` / `Zlink.CreateTimer()`

Creates a reusable poller, or a standalone timer.

```csharp
using IPoller poller = Zlink.CreatePoller();
using IZlinkTimer timer = Zlink.CreateTimer();
```

**Options.** Neither factory takes parameters (`CreateTimer(ISpot)` also exists, binding a timer
to a Spot's lifecycle — Service category).

**Completion result.** Both return their resource interface (`IPoller`, `IZlinkTimer`)
synchronously; the caller owns and must dispose each.

**When to use.** See the Eventing category for each interface's own operations — this entry only
covers creation.

---

## `Zlink.UnhandledCallbackException`

A static event raised when a user callback throws.

```csharp
Zlink.UnhandledCallbackException += ex => logger.LogError(ex, "callback failed");
```

**Options.** Subscribes/unsubscribes an `Action<Exception>`.

**Completion result.** Synchronous add/remove. The event fires on the background dispatch thread
running the callback that threw, since that thread can't propagate the exception to its original
caller.

**When to use.** Subscribe to observe exceptions from any registered callback (stream packet,
monitor, poll, SPOT dispatch, request/reply — Sockets/Eventing/Service categories) that would
otherwise be silently lost.

---

See [`Contracts/Core/`](../../../../bindings/dotnet/src/Zlink/Contracts/Core/) and the
[.NET binding spec](../../spec/dotnet/README.en.md) for the full rationale.
