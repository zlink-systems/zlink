# HTTP Client — Common Spec

[Spec table of contents](../server/README.en.md) | [Previous: Channel Messaging](../server/02-channel-transport/02-channel-messaging.en.md) | [Next: SPOT Messaging](../server/03-spot-actor/02-spot-messaging.en.md)

> This document defines the boundary for registering and calling an
> HTTP client in Framework. It owns identity, the fluent builder form,
> the execution terminator, the combination with the Spot execution
> context, and the codec and common error model.
>
> The detailed contract (builder, response, redirect/retry/cookie,
> auth/TLS/proxy, compression, error mapping, regression) is each
> owned by [01-11](README.en.md) in the same folder.
>
> The per-language names of the common concepts are owned by
> [language-interfaces](language-interfaces.en.md); the exact parameter and
> return types of those names are owned by [`languages/<lang>/`](README.en.md),
> which project that table.

## 1. Identity — A Framework Companion Client

**The HTTP client sits in the same place as the STREAM connector.** It's
distributed as a separate package, but it's a **framework-dedicated
companion client**, and the contract is owned by framework.

| | [STREAM Connector](../server/00-foundation/02-glossary.en.md#stream-connector) | HTTP Client |
|---|---|---|
| Package | Separate | Separate |
| Contract ownership | Framework common spec ([32](../stream-connector/32-stream-connector.en.md)) | Framework common spec (this document) |
| Per-language interface | `stream-connector/languages/<lang>/` | `http-client/languages/<lang>/` |

**There's exactly one reason it exists** — a framework application must
be able to call an **external API and legacy API in zlink style**. It
isn't meant to replace a general-purpose HTTP library.

[01 Scope And Architecture §1.3](01-scope-and-architecture.en.md#13-relationship-with-framework--one-way-dependency) owns the Framework contracts consumed by the HTTP client and the per-language
deliverable boundaries. The [Spot](../server/00-foundation/02-glossary.en.md#spot)
execution context is connected through the execution scheduler injection point in
[§3.2](#32-the-turn-seam--a-single-injection-point).

## 2. Fluent Builder

**Same form as framework messaging** — "select operation → configure →
execution-mode terminator".

```
client.post("/games")               // operation
      .header("x-request-id", ...)  // configure
      .query("region", "kr")
      .body(createGameReq)
      .timeout(3s)
      .submit<CreateGameRes>()      // response completion terminator — the Java/Node name.
                                    // Per-language names: Language interfaces §1.4
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

## 3. Execution Terminator — Response Completion (+ Callback)

The HTTP client's completion surface is response completion, callback, and
the DI server builder's `Yield`. Terminator names, response shapes and the
absence of one-way are owned by
[Language interfaces §1.4](language-interfaces.en.md#14-terminators).

| Execution Mode | What It Waits For | Spot Execution Queue |
|---|---|---|
| **response completion** | Waits until the HTTP response arrives | Keeps the current turn |
| **`Yield`** (DI server builder) | Waits until the HTTP response arrives | Gate return and resume follow [Submit and completion §2](../server/01-execution/01-submit-and-completion.en.md#2-completion-meaning-per-terminator-and-per-language-names) |

**Callback is a separate completion path** for a caller that doesn't
use an awaitable (a CLI, an event-loop-based client). The HTTP client
provides that path together.

Using a callback in a Spot execution context proceeds without waiting
for the call, and the completion callback is queued as a **new turn**
of that Spot execution queue. If the completion value must continue
the same turn's judgment, use the per-language response completion
terminator instead of a callback.

### 3.1 How To Return The Spot Gate While Waiting For External HTTP

The execution contexts that have `Yield` are owned by
[Submit and completion §2](../server/01-execution/01-submit-and-completion.en.md#2-completion-meaning-per-terminator-and-per-language-names);
the two usage forms — the DI server builder's `Yield`, and an I/O Worker
with the Worker `Yield` — and their examples are owned by
[05 §5.2](05-execution-model.en.md#52-external-http-wait-and-the-spot-execution-queue).
The HTTP package does not judge the Spot execution context. Framework injects
the current execution turn at DI registration, and the DI server builder's
`Yield` returns that turn ([turn seam](#32-the-turn-seam--a-single-injection-point)).

### 3.2 The Turn Seam — A Single Injection Point

**The HTTP client depends on the framework error kinds and codec and holds no
Spot turn information.** Framework connects the current turn to the single
**execution scheduler** it injects at DI registration.

- The HTTP client puts an **execution scheduler injection point** as a
  public contract. The scheduler decides where to resume completion.
- **Framework injects the callback completion scheduler at DI
  registration.** The callback enters as a new turn of the original
  Spot execution queue.
- The DI server builder's `Yield` returns the injected turn and resumes in a
  new turn on the same execution line.

The C++ HTTP client expresses the same scheduler seam with
`coroutines(resume_scheduler)` and `framework_resume_scheduler_t`.

### 3.3 A Runtime Execution Context Rejects Blocking Terminator Calls

**Whether a language provides a public terminator that synchronously unwraps
the completion value, and under what name, is owned by
[Language interfaces §1.4](language-interfaces.en.md#14-terminators).** Calling a
blocking terminator in a runtime execution context fails immediately with
`InvalidOperation`, per
[Submit and completion §2](../server/01-execution/01-submit-and-completion.en.md#2-completion-meaning-per-terminator-and-per-language-names).
In a language without a blocking terminator, a test or CLI that must wait
synchronously wraps the call with a language idiom (`GetAwaiter().GetResult()`,
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
| **DI-injected client** | **Spot handler · server code** | response completion / callback / `Yield` |

## 5. Codec

**The HTTP client shares a codec extension with framework, but keeps a
separate registry instance**
([Stream Session §5](../server/04-session/01-stream-session.en.md#6-payload-conversion-and-the-codec-boundary)).
The same codec extension object can be registered on both, but
**registration must be done separately per host.**

Typed body encode/decode is handled by that registry. The raw body API
doesn't go through the registry.

## 6. Error Model

**The HTTP client doesn't build its own exception hierarchy.** It uses
the framework common error model's
([Framework Error Model](../server/00-foundation/07-framework-error-model.en.md)) error kind
as is. **It doesn't create a new HTTP-client-dedicated error kind.**

| Situation | Kind |
|------|------|
| Configuration/usage error, typed decode, decompression, or redirect format error | `ProtocolError` |
| Network, DNS, proxy, and target connection failure | `Unavailable` |
| Configured response body byte limit exceeded | `Rejected` |
| Per-attempt timeout exceeded | `DeadlineExceeded` |
| HTTP status 400 or above, or an unclassifiable execution failure | `InternalFailure` |

Detailed mapping is owned by [09 Error Model](09-error-model.en.md).

## 7. Regression Test

| Item | Verification |
|---|---|
| Terminator axis | Each language's public terminators match the table in [Language interfaces §1.4](language-interfaces.en.md#14-terminators) exactly — no name outside the table, no name from the table missing |
| Turn preservation | While a Spot handler waits for response completion, another callback of the same Spot doesn't start |
| Turn return | Only the server builder's `Yield` and `RunIoWorker` + Worker `Yield` return the shared Spot gate; response completion does not |
| Surface limit | The standalone HTTP request builder has no `Yield`, and the C++ blocking terminators fail with `InvalidOperation` in a runtime execution context |
| Registration | The server surface is obtained only through DI injection, and a client isn't built with a static factory inside a handler |
| Error kind | There's no HTTP-client-dedicated kind — only framework common kind is used |
| builder | Mixing body sources fails with `ProtocolError` |
