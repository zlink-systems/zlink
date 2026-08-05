[한국어](01-core.ko.md) | English

[Reference index](README.en.md)

# 01. Core

This category covers the context lifecycle, context options, routing identity, and the `Zlink`
factory/utility class — the library's process-wide entry points and utility resources. Socket
creation methods on `Context` are listed here for completeness but detailed under the Sockets
category; poller/timer creation on `Zlink` is listed here but detailed under the Eventing
category. Kotlin shares this same runtime — there is no separate Kotlin contract source for Core.
The exact signatures are owned by
[`contracts/core/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/core/).

---

## `Zlink.createContext()`

Creates a messaging context — the factory and owner of sockets, and the prerequisite for every
other entry in this reference.

```java
try (Context context = Zlink.createContext()) {
    // ...
}
```

**Options.** No parameters.

**Completion result.** Returns `Context` synchronously. The caller owns it and must `close()` it
(`Context extends AutoCloseable`); closing it terminates anything still open under it, including
sockets created from it.

**When to use.** Once per context the application needs; most applications need exactly one.

---

## `Context.shutdown()` / `Context.recalculateAutoHwm()`

Interrupts blocking operations on the context's sockets without closing them, or forces an
immediate recalculation of automatic high-water marks.

```java
context.shutdown();
context.recalculateAutoHwm();
```

**Options.** Neither takes parameters.

**Completion result.** Both synchronous, `void`. `shutdown()` interrupts blocking calls on sockets
under this context but doesn't close the context or its sockets. `recalculateAutoHwm()` recomputes
automatic HWM only for sockets still configured with an `AutoHwmProfile` (Sockets category).

**When to use.** Call `shutdown()` before closing a context with sockets in use across multiple
threads, to avoid a thread blocking indefinitely. Pair `recalculateAutoHwm()` with an
`AutoHwmProfile` change to apply new sizing immediately.

---

## `Context.options()` / `ContextOptions`

The context-wide options facade, governing I/O threads and the defaults every socket created from
the context inherits. `ContextOptions` has a public constructor (`new ContextOptions(context)`),
but `context.options()` is the normal path.

```java
context.options().ioThreads(8);
context.options().autoHwmProfile(AutoHwmProfile.LOW_LATENCY);
context.options().addThreadAffinityCpu(2);
```

**Options.**

| Member | Type | Meaning |
| --- | --- | --- |
| `ioThreads()`/`ioThreads(int)` | `int` | I/O thread count |
| `maxSockets()`/`maxSockets(int)` | `int` | context-wide socket cap |
| `socketLimit()` | `int`, read-only | build's hard cap on `maxSockets` |
| `threadPriority()`/`threadPriority(int)` | `int` | dispatch thread priority |
| `threadSchedulingPolicy()`/`threadSchedulingPolicy(int)` | `int` | dispatch thread scheduling policy |
| `threadNamePrefix()`/`threadNamePrefix(String)` | `String` | OS-visible dispatch thread name prefix; the getter returns the value last set on this facade instance, not a native read-back |
| `maxMessageSize()`/`maxMessageSize(int)` | `int` | per-message size cap |
| `messageThreadSize()` | `int`, read-only | native message struct size, diagnostic only |
| `blocky()`/`blocky(boolean)` | `boolean` | whether blocking calls actually block vs. fail fast |
| `autoHwmEnabled()`/`autoHwmEnabled(boolean)` | `boolean` | whether auto-HWM sizing is active |
| `autoHwmRecalcDebounce()`/`autoHwmRecalcDebounce(Duration)` | `Duration` | minimum interval between automatic recalculations |
| `autoHwmProfile()`/`autoHwmProfile(AutoHwmProfile)` | `AutoHwmProfile` | automatic HWM sizing profile — see Sockets category |
| `autoHwmMessageUnitBytes()`/`autoHwmMessageUnitBytes(long)` | `long` (unsigned 64-bit bit pattern) | accounted-byte unit for auto-HWM; `0` selects the socket-type default |
| `addThreadAffinityCpu(int)` | — | pins an I/O thread to a CPU (setter-only) |
| `removeThreadAffinityCpu(int)` | — | unpins an I/O thread from a CPU (setter-only) |

**Completion result.** Every getter/setter is synchronous.

**When to use.** Adjust before creating sockets when the defaults don't fit the deployment. Pair
an `autoHwmProfile`/`autoHwmEnabled` change with `Context.recalculateAutoHwm()` to apply it
immediately.

---

## `Context.createPairSocket()` / `createDealerSocket()` / `createRouterSocket()` / `createPubSocket()` / `createSubSocket()` / `createXPubSocket()` / `createXSubSocket()` / `createStreamSocket()`

Creates a socket of the given type, owned by the caller.

```java
try (DealerSocket dealer = context.createDealerSocket()) {
    // ...
}
```

**Options.** None of the eight factory methods takes parameters — each returns its matching
interface (`PairSocket`, `DealerSocket`, `RouterSocket`, `PubSocket`, `SubSocket`, `XPubSocket`,
`XSubSocket`, `StreamSocket`).

**Completion result.** Synchronous. The caller owns and must `close()` the returned socket
independently of the context.

**When to use.** See the Sockets category for each interface's operations and options — this
entry only covers creation.

---

## `RoutingId`

A binary-safe value type identifying a messaging peer or route, 1 to 255 bytes (`MAX_LENGTH`, a
public constant). Internally maintains a per-thread trusted-bytes cache to avoid reallocating on
the receive hot path — not part of the public contract surface.

```java
RoutingId fromString = RoutingId.from("worker-3");
RoutingId fromBytes = RoutingId.from(rawBytes);
RoutingId fromRange = RoutingId.from(buffer, offset, length);
RoutingId fromUint = RoutingId.from(42L);
RoutingId fromUuid = RoutingId.from(UUID.randomUUID());
RoutingId restored = RoutingId.fromHex(previouslyPrinted.toHex());
```

**Options.**

| Member | Meaning |
| --- | --- |
| `from(byte[])` | copies the full array as-is |
| `from(byte[] value, int offset, int length)` | copies the selected byte range — a Java-specific overload not present in dotnet/cpp |
| `from(String)` | UTF-8 encode |
| `from(long)` | 4-byte big-endian from an unsigned 32-bit value; throws `IllegalArgumentException` if the value doesn't fit in 32 bits |
| `from(UUID)` | 16-byte big-endian |
| `fromHex(String)` | restores bytes `toHex()` printed |
| `toBytes()` | defensive copy of the bytes |
| `size()` | byte length, 1-255 |
| `toHex()` | hex encoding, round-trippable with `fromHex` |
| `toString()` | display form: printable UTF-8, then 4-byte-as-unsigned-int, then 16-byte-as-UUID, then a `hex:`-prefixed fallback |
| `equals`/`hashCode` | value equality |

**Completion result.** Every factory and accessor is synchronous. Out-of-range length throws
`IllegalArgumentException`; a malformed hex string to `fromHex` throws the same.

**When to use.** `from(String)` for a human-assigned identity, `from(long)`/`from(UUID)` for a
numeric or UUID-shaped identity, and the raw byte overloads (including the range overload) when
the identity is already binary or a slice of a larger buffer. `toHex()`/`fromHex()` for a durable
round trip — `toString()` is display-only.

---

## `Zlink.strerror(int)` / `Zlink.has(String)` / `Zlink.version()` / `ZlinkVersion.get()`

Converts a native error code to a message, checks for an optional build capability, or reads the
native library's build version.

```java
String message = Zlink.strerror(errnum);
boolean hasTls = Zlink.has("tls");
int[] version = Zlink.version();
```

**Options.**

| Member | Meaning |
| --- | --- |
| `strerror(int errnum)` | the message text for that native error code |
| `has(String capability)` | whether the named optional capability is compiled into this build — recognized names are `"tcp"`, `"ipc"`, `"tls"`, `"ws"`, `"wss"`; any other string returns `false` |
| `version()` / `ZlinkVersion.get()` | `int[]` of `{major, minor, patch}`; equivalent — `ZlinkVersion` is a thin convenience wrapper delegating to `Zlink.version()` |

**Completion result.** All synchronous. **`Zlink.errno()` exists in source but has no `public`
modifier** — it is not reachable from application code.

**When to use.** `version()` to confirm a dynamically-loaded native library matches expectations.
`has(...)` at startup to branch on optional transports. `strerror` for diagnostics alongside a
native error code surfaced elsewhere (Errors category).

---

## `Zlink.createAtomicCounter()` / `Zlink.createStopwatch()` / `Zlink.createThread(Runnable)`

Creates a thread-safe integer counter, a high-resolution stopwatch, or a running background
thread.

```java
try (AtomicCounter counter = Zlink.createAtomicCounter()) {
    int newValue = counter.increment();
}

try (ZlinkStopwatch watch = Zlink.createStopwatch()) {
    Duration partial = watch.intermediate();
    Duration total = watch.stop();
}

try (ZlinkThread thread = Zlink.createThread(() -> doWork())) {
    thread.join();
}
```

**Options.**

| Member | Meaning |
| --- | --- |
| `createAtomicCounter()` | no parameters |
| `AtomicCounter.set(int)` | assigns the counter's value |
| `AtomicCounter.increment()`/`decrement()` | adjusts the counter by one, returning the *new* value |
| `AtomicCounter.value()` | reads the current value |
| `createStopwatch()` | no parameters |
| `ZlinkStopwatch.intermediate()` | elapsed `Duration` since construction, callable any number of times |
| `ZlinkStopwatch.stop()` | elapsed `Duration` since construction, called exactly once to finish |
| `createThread(Runnable task)` | runs `task` immediately on the new thread |
| `ZlinkThread.join()` | blocks until the task finishes |

**Completion result.** All three factories return their resource synchronously; the caller owns
and must `close()` each (all three extend `AutoCloseable`).

**When to use.** `createAtomicCounter` for a shared count safe across threads. `createStopwatch`
for benchmarking. `createThread` for a portable background thread instead of `java.lang.Thread`
directly, when the zlink runtime needs to own its lifecycle.

---

## `Zlink.proxy(...)` / `Zlink.proxySteerable(...)` / `Zlink.sleep(Duration)`

Runs a bidirectional message-forwarding loop between two sockets (optionally steerable via a
control socket), or sleeps the calling thread.

```java
Zlink.proxy(frontend, backend, capture); // capture may be null; blocks until context termination
Zlink.proxySteerable(frontend, backend, capture, control);
Zlink.sleep(Duration.ofSeconds(1));
```

**Options.**

| Member | Meaning |
| --- | --- |
| `proxy(Socket frontend, Socket backend, Socket capture)` | `capture` may be `null` |
| `proxySteerable(Socket frontend, Socket backend, Socket capture, Socket control)` | adds a required `control` socket |
| `sleep(Duration)` | blocks the calling thread |

**Completion result.** All three synchronous, no return value. `proxy`/`proxySteerable` block the
calling thread until the context terminates (or, for `proxySteerable`, until a control command or
error ends the loop) — run either on a dedicated thread. **Only `sleep(Duration)` is public** —
`Zlink.sleep(int seconds)` and `Zlink.multipartClose(Message[])` exist in source but have no
`public` modifier and are not reachable from application code, unlike dotnet's public
`Zlink.Sleep(TimeSpan)`/`Zlink.MultipartClose(...)` pairing.

**When to use.** `proxy` for a simple fire-and-forget forwarding loop. `proxySteerable` when the
loop needs to be paused/resumed/terminated from another thread via `control`. Since there is no
public `multipartClose`-equivalent helper, close each part individually via `Message.close()`
(Messaging category).

---

See [`contracts/core/`](../../../../bindings/java/src/main/java/systems/zlink/contracts/core/)
and the [Java binding spec](../../spec/java/README.en.md) for the full rationale.
