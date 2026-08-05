# HTTP Client — Common Spec

[Spec table of contents](../README.en.md) | [Previous: Channel Messaging](../08-channel-messaging.en.md) | [Next: SPOT Messaging](../12-spot-messaging.en.md)

> This document defines the boundary for registering and calling an
> HTTP client in Framework. It owns identity, the fluent builder form,
> the execution terminator, the combination with the Spot execution
> context, and the codec and common error model.
>
> The detailed contract (builder, response, redirect/retry/cookie,
> auth/TLS/proxy, compression, error mapping, regression) is each
> owned by [01-11](README.en.md) in the same folder.
>
> The exact per-language type and signature is owned by
> [`languages/<lang>/`](README.en.md).
> [language-interfaces](language-interfaces.en.md) is a
> **non-normative cross-reference table** viewing the five languages
> side by side — it doesn't fix a contract.

## 1. Identity — A Framework Companion Client

**The HTTP client sits in the same place as the STREAM connector.** It's
distributed as a separate package, but it's a **framework-dedicated
companion client**, and the contract is owned by framework.

| | [STREAM Connector](../01-glossary.en.md#stream-connector) | HTTP Client |
|---|---|---|
| Package | Separate | Separate |
| Contract ownership | Framework common spec ([32](../stream-connector/32-stream-connector.en.md)) | Framework common spec (this document) |
| Per-language interface | `stream-connector/languages/<lang>/` | `http-client/languages/<lang>/` |

**There's exactly one reason it exists** — a framework application must
be able to call an **external API and legacy API in zlink style**. It
isn't meant to replace a general-purpose HTTP library.

**The dependency is one-way: HTTP client → framework contract.** The
HTTP client consumes framework's **error kind** and **codec extension**
(§5, §6), and doesn't build its own exception hierarchy.
**Framework doesn't depend on the HTTP client** — it works without the
HTTP client.

**What it consumes is framework's contract, not its runtime.** That way
a CLI and client scenario doesn't drag in the framework host/runtime.
The combination with the [Spot](../01-glossary.en.md#spot) execution
context happens only through §3.2's **single injection point** — if the
scheduler isn't injected, the HTTP client is an ordinary client that
doesn't know about turns.

The deliverable boundary and per-language package split is owned by
[01 Scope And Architecture §1.3](01-scope-and-architecture.en.md).

## 2. Fluent Builder

**Same form as framework messaging** — "select operation → configure →
execution-mode terminator".

```
client.post("/games")               // operation
      .header("x-request-id", ...)  // configure
      .query("region", "kr")
      .body(createGameReq)
      .timeout(3s)
      .submit<CreateGameRes>()      // C++/Java's response completion terminator
                                    // Node uses async<CreateGameRes>()
```

- 7 verbs: `get` / `post` / `put` / `delete` / `patch` / `head` /
  `options`.
- Configuration axis: `header`, `query`, `timeout`, body source
  (typed / raw / streaming / form / multipart).
- **Body sources are mutually exclusive.** Mixing them is
  `ProtocolError`.

The builder's detailed contract (path format, percent-encoding,
per-body-source retry availability, etc.) is owned by
[03 Request Builder](03-request-builder.en.md).

## 3. Execution Terminator — One-Way And Response Completion (+ Callback)

The HTTP client's completion surface is one-way submission, response
completion, and callback. The exact name is `.NET`'s `Async`, Kotlin
wrapper's `await`, Java/C++'s `submit`. Node uses `submitRaw` for raw
response, `async` for typed response and callback, and `submit` for
one-way. TypeScript inheritance signature constraints are owned by each
language's exact interface. `Yield`, which returns the shared Spot
gate, is only provided to a server request and Worker call, and isn't
included in the HTTP request builder
([04 §1.1](../05-async-execution-policy.en.md)).

| Execution Mode | What It Waits For | Spot Execution Queue |
|---|---|---|
| **one-way submission** | Waits until the HTTP request is submitted at the transport boundary | Keeps the current turn. No normal completion value |
| **response completion** | Waits until the HTTP response arrives | Keeps the current turn |

**Callback is a separate completion path** for a caller that doesn't
use an awaitable (a CLI, an event-loop-based client). The HTTP client
provides that path together.

Using a callback in a Spot execution context proceeds without waiting
for the call, and the completion callback is queued as a **new turn**
of that Spot execution queue. If the completion value must continue
the same turn's judgment, use the per-language response completion
terminator instead of a callback.

### 3.1 How To Return The Spot Gate While Waiting For External HTTP

An HTTP client call itself doesn't return the shared Spot gate. If
another Spot work item must proceed while waiting for an external API
during Actor entry/exit, run the HTTP client's response completion
terminator in an I/O Worker and wait with the Worker call's `Yield`.

```csharp
var profile = await Context
    .RunIoWorker(async workerCancellation =>
        await http.Get($"/players/{id}").Fetch<Profile>(workerCancellation))
    .Yield(ct);
```

The HTTP request builder doesn't provide a `Yield` terminal. Gate
return and reacquisition is owned by the server runtime's Worker call,
so the HTTP package doesn't judge the Spot execution context.

### 3.2 The Turn Seam — A Single Injection Point

**The HTTP client knows framework's error kind and codec, but not the
Spot's turn.** The only thing that knows the turn is a single
**injected execution scheduler**.

- The HTTP client puts an **execution scheduler injection point** as a
  public contract. The scheduler decides where to resume completion.
- **Framework injects the callback completion scheduler at DI
  registration.** The callback enters as a new turn of the original
  Spot execution queue.
- Neither DI nor standalone use exposes `Yield` on the HTTP request
  builder. Only the per-language response completion terminator and
  callback are used.

The C++ HTTP client expresses the same scheduler seam with
`coroutines(resume_scheduler)` and `framework_resume_scheduler_t`.

### 3.3 Doesn't Put A Blocking Terminator

**Doesn't build a public terminator that synchronously unwraps the
completion value**
([04 §2](../05-async-execution-policy.en.md)). A blocking alternative
terminator of the same meaning is a contract violation. If a
synchronous wait is needed in a test or CLI, the caller wraps it
directly with a language idiom (`GetAwaiter().GetResult()`,
`runBlocking`, `.join()`).

## 4. Server Surface And Registration

**The HTTP client used in a server (Spot handler/channel handler) is
injected through DI.** Don't build a client with a static factory
inside a handler — it loses the connection pool and turn seam.

- **The application registers it by name.** Since baseUrl, auth,
  timeout, and retry policy differ per service, framework doesn't
  auto-register one default client.
- The registration surface's form is the same as channel registration:
  name and policy are registered together at the configuration stage,
  and a handler is injected by that name.
- **The static factory entry point stays client-side-only.** Used by a
  CLI and client scenario.

| Surface | Who Uses It | Terminator |
|------|-----------|------------|
| Static factory | CLI · client scenario | response completion / callback |
| **DI-injected client** | **Spot handler · server code** | one-way / response completion / callback |

## 5. Codec

**The HTTP client shares a codec extension with framework, but keeps a
separate registry instance**
([Stream Session §5](../19-stream-session.en.md#5-codec-layer-separation)).
The same codec extension object can be registered on both, but
**registration must be done separately per host.**

Typed body encode/decode is handled by that registry. The raw body API
doesn't go through the registry.

## 6. Error Model

**The HTTP client doesn't build its own exception hierarchy.** It uses
the framework common error model's
([Framework Error Model](../32-framework-error-model.en.md)) error kind
as is. **It doesn't create a new HTTP-client-dedicated error kind.**

| Situation | Kind |
|------|------|
| Configuration/usage error, typed decode, decompression, or redirect format error | `ProtocolError` |
| Network, DNS, proxy, and target connection failure | `Unavailable` |
| Configured response body byte limit exceeded | `CapacityExceeded` |
| Per-attempt timeout exceeded | `DeadlineExceeded` |
| HTTP status 400 or above, or an unclassifiable execution failure | `InternalFailure` |

Detailed mapping is owned by [09 Error Model](09-error-model.en.md).

## 7. Regression Test

| Item | Verification |
|---|---|
| Terminator axis | Has one-way and response completion and callback completion paths, and **doesn't have** a blocking-unwrap terminator or an HTTP `Yield` |
| Turn preservation | While a Spot handler waits for response completion, another callback of the same Spot doesn't start |
| Turn return | Only returns the shared Spot gate when HTTP response completion runs inside `RunIoWorker` and waits with the Worker `Yield` |
| Surface limit | Neither DI nor standalone use exposes `Yield` on the HTTP request builder |
| Registration | The server surface is obtained only through DI injection, and a client isn't built with a static factory inside a handler |
| Error kind | There's no HTTP-client-dedicated kind — only framework common kind is used |
| builder | Mixing body sources fails with `ProtocolError` |
