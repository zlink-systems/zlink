# ZLink System Development Principles

> The unique development principles shared across the whole ZLink project — the core,
> the bindings, and applications built on top of ZLink. Doesn't conflict with
> [POSDDD](./posddd.md); it builds on top of it. This document covers only what POSDDD
> doesn't: ZLink's own structural facts, domain vocabulary, contracts, and team
> policies.
>
> `AGENTS.md`'s "Framework public contract parity" is the authority on cross-language
> parity policy for the public contract. This document only covers how that policy is
> upheld from a design-principle point of view — if the policy itself changes, fix
> `AGENTS.md`.
>
> **Reader**: a developer building the ZLink core/bindings, or building an application
> on top of ZLink. Looking for an answer to "what's true in ZLink, and what's policy."

This document is divided into the following parts.

- **Architecture Structure** — the layered structure of the core, bindings, and
  framework, and the hexagonal structure of applications built on ZLink.
- **Domain Vocabulary and Public Contract** — the vocabulary unique to this codebase
  (RoutingId, handle, public contract), and the contracts that vocabulary must uphold.
- **Code and Testing Rules** — rules specific to systems-software development: comment
  placement, test coverage, refactoring and regression testing.

---

# Part 1 — Architecture Structure

## Core, Bindings, Framework: Layered + Public Contract/Runtime Separation

The public contract has to stay stable across languages for a long time, while
transport/codec/platform detail needs to stay free to change. So the ZLink core and
bindings themselves use layered architecture with public contract/runtime separation.

```text
+--------------------------------+
| Public Contract                |
| API, ABI, Spec, Bindings       |
+--------------------------------+
              v
+--------------------------------+
| Runtime Boundary                |
| Lifecycle, State, Ownership    |
+--------------------------------+
              v
+--------------------------------+
| Integration Layers             |
| Transport, Codec, Platform     |
+--------------------------------+
```

This separation doesn't stop at the core — core, bindings, and framework independently
repeat the same pattern across all three boundaries. Each layer only knows the layer
below's **published public surface**, and knows nothing of that layer's runtime
internals.

```text
+---------------------------------+
| framework/languages/<lang>      |
|  Public Contract  |  Runtime    |
+---------------------------------+
    depends on published package
    (NuGet, npm, Maven — not source)
              v
+---------------------------------+
| bindings/<lang>                 |
|  Public Contract  |  Runtime    |
+---------------------------------+
    depends on published artifact
    (installed headers + shared library — not core/src)
              v
+---------------------------------+
| core                            |
|  Public Contract  |  Runtime    |
|  (core/include/)  | (core/src/) |
+---------------------------------+
```

- **core**: `core/include/` (the public headers, with `zlink.h` as the single
  reference) is the public contract, and `core/src/` is the runtime. No code outside
  core includes `core/src/` directly.
- **bindings/&lt;lang&gt;**: depends only on core's public headers and the built shared
  library (as distributed in a package). Each binding also internally splits its
  public surface (Contracts) from its implementation (Runtime) — the reference for this
  folder-structure alignment is the `.NET` binding's `Contracts`/`Runtime` split
  (`bindings/doc/spec/README.ko.md`). The reference implementation for core's actual
  behavior is core (C/C++) itself; `.NET` doesn't replace that.
- **framework/languages/&lt;lang&gt;**: depends on the published package (NuGet
  `Systems.Zlink`, npm `@zlink-systems/zlink`, Maven `systems.zlink:zlink`), not the
  binding's source. No framework code references a binding's internal implementation.

## Applications Built on ZLink: Hexagonal

Applications built using ZLink (samples, game servers, etc.) use hexagonal
architecture, because business rules and use cases need to outlive concrete
technologies like ZLink, Redis, or HTTP.

A sample's own logic is simple enough to work fine without hexagonal architecture. The
reason samples still use this structure is that what a sample needs to show isn't only
"how do you call the ZLink API." This document guides not just how to use the
framework, but also the architecture pattern for placing that framework inside a real
application — if a reader starts by copying this structure directly, they avoid, from
the outset, the situation where ZLink calls get mixed into business rules as the code
grows.

The source structure doesn't put adapter implementations inside application.

```text
Server/<role>/
  Domain/
  Application/
  Infrastructure/
    ZLink/
    Redis/
    Http/
```

| Directory | Responsibility |
|---|---|
| `Domain/` | Business rules, state transitions, value objects, domain events |
| `Application/` | Use cases, application services, port interfaces |
| `Infrastructure/` | Framework callbacks, Redis, HTTP, storage implementations, host wiring |

`Application/` doesn't reference `Infrastructure/` types directly. When an external
capability is needed, application defines a port interface, and `Infrastructure/`
implements that interface. Code attached to framework callbacks — ZLink handlers,
Spots, Actors — lives in `Infrastructure/ZLink/`. Implementations of a specific
external technology — Redis, files, HTTP — are split into technology-specific
subdirectories like `Infrastructure/Redis/`, `Infrastructure/Http/`. Existing
codebases using the name `Adapters/` mean the same thing, but new samples or structure
cleanups should prefer `Infrastructure/`.

The dependency direction runs outside-in.

```text
+--------------------------------+
| Infrastructure                 |
| ZLink, HTTP, Queue, DB, API    |
+--------------------------------+
              | port
              v
+--------------------------------+
| Application Use Cases          |
+--------------------------------+
              v
+--------------------------------+
| Domain Model                   |
| Aggregate, Entity, Value       |
+--------------------------------+
```

In both structures, if the port and adapter only forward the request, the layer is
shallow. Either remove it, or hide the external technology's detail behind a deep
interface that's easy for application to use, and grow its responsibility.

---

# Part 2 — Domain Vocabulary and Public Contract

## The Domain of Systems Software

Where enterprise software's domain is business concepts like orders, payments, and
customers, the ZLink core's domain is systems concepts a user has to understand
precisely — context, handle, socket, message, buffer, ownership, lifecycle, timeout,
cancellation, error code. The public contract carries only the meaning of these
concepts that the caller needs to know — runtime data structures, queue
implementation, transport wiring, and codec detail stay hidden and don't leak into the
contract. If the public API and the runtime boundary pass through the same name and
the same behavior unchanged, one of the two is unnecessary or the responsibilities are
split wrong.

## Public API Design Checklist

When designing a new public API or contract, settle the following first. (Instead of
carrying over business-DDD names like `ContextAggregate`, `SocketRepository`, or
`MessageDomainService` unchanged, answer the questions below first.)

- Which object owns the lifecycle?
- Who releases the memory and the handle?
- What calls are valid after close/destroy/move?
- Does the name for the same concept stay the same across every public API, binding,
  and doc?
- Does the error code consistently express the state transition and the caller's
  responsibility?
- Does the meaning of timeout/cancellation/backpressure/reconnect stay consistent
  across layers?

A deep system API absorbs the internal complexity of these decisions, and exposes only
a simple lifecycle and a consistent error contract to the caller.

## Cross-Language Name Consistency

The general document's "same concept → same name" rule applies in ZLink not just
**within a single language** but **across the entire set of language bindings**. The
same system concept (RoutingId, Spot, Actor, ownership transfer, etc.) uses the same
name in the docs and APIs of core, cpp, dotnet, java, kotlin, node, rust, and python.
A name that differs by language is itself information leakage — a user has to relearn
the mapping every time they cross languages.

The criteria for deciding whether to create a new per-language public contract or
follow an existing one (whether it's grounded in a spec/guide document, the fact that a
single language's implementation alone isn't grounds for a new public contract, etc.)
follow `AGENTS.md`'s "Framework public contract parity."

## A Real Case — Designing Socket Handle Close (`zlink_close`)

To see how the above concepts all apply together to one real public API, walk through
`zlink_close()` from start to finish.

**Event storming.** The events a `zlink_close()` call produces, and the conditions that
separate them:

- `SocketClosed` — normal close. Result `ZLINK_CLOSE_OK`.
- `CloseRejectedBusy` — the same handle has an in-flight callback or an already-admitted
  API call, so it can't be closed right now. Result `ZLINK_CLOSE_BUSY` (errno
  `EBUSY`/`EDEADLK`).
- `CloseRejectedAlreadyShutdown` — a close was requested again on an already-closed
  handle. Result `ZLINK_CLOSE_SHUTDOWN` (errno `ESHUTDOWN`).
- `CloseRejectedInvalidHandle` — the handle is NULL or invalid. Result
  `ZLINK_CLOSE_INVALID_HANDLE` (errno `EFAULT`/`ESTALE`).

The command is the `zlink_close(handle)` call itself, and the actor is the application
thread that was using that handle.

**Entity and invariant.** The socket handle is an entity — identified by pointer, and
its state changes from creation to close. "Can it be closed right now" is an invariant
attached to that handle: if the same handle has an in-flight callback or an admitted
API call, it can't transition to closed. The only path that upholds this invariant is
`zlink_close()` itself — the caller has no way to compute "is it closeable right now"
directly; the handle encapsulates that judgment.

**Error-handling decisions.** All three failure events surface as-is, unmasked —
especially BUSY, which is retryable, but since the caller has to decide when to retry
(the internals can't know which callback it's waiting on, or how long), it isn't
masked into a silent retry. Conversely, closing your own handle **from inside** a
send-ready or monitor callback was redefined so it isn't an error — it's deferred until
after the callback finishes. This is a real application of "define errors out of
existence" (Part 1). Instead of turning the common pattern of self-close inside a
callback into an error, it was redefined as "handled after the callback ends,"
eliminating the error entirely.

**Bounded context — the same "close" has a different contract strength at different
layers.** Calling `zlink_close()` again on an already-closed socket handle is filtered
into `ZLINK_CLOSE_SHUTDOWN` — a contract the runtime upholds for you. By contrast, using
a context again after `zlink_ctx_term()` is itself prohibited, and doing so is
undefined behavior — there's no runtime that filters this for you; the caller has to
uphold the contract itself. The fact that the same word "close" has a different
contract strength at the socket layer versus the context layer is a real case of
bounded context — whether the same word's meaning genuinely changes across a boundary.

**Name consistency.** `ZLINK_CLOSE_BUSY`, `ZLINK_CLOSE_SHUTDOWN`, and
`ZLINK_CLOSE_INVALID_HANDLE` are the same `zlink_close_result_t` used not just by
`zlink_close` but also by `zlink_ctx_term` and `zlink_ctx_shutdown` — the concept of "a
close result" is expressed with the same name and the same representation whether it's
a socket or a context.

---

# Part 3 — Code and Testing Rules

## Comment Placement: Public Contract vs. Implementation Rationale

[`source-comment-principles.ko.md`](../source-comment-principles.ko.md) is the
authority on the format and criteria for public API and internal comments. ZLink covers
a C/C++ core where declaration and implementation live in separate files, together with
per-language bindings, so that document's rules apply as-is — the public contract
(ownership, error conditions, the meaning of timeout/cancellation, units, the meaning of
null) goes in the header (`.h`/`.hpp`) or the language's equivalent public declaration
(IDL, `.d.ts`, an interface file, etc.), and the rationale for the implementation (why
it was written this way, non-obvious tradeoffs, working around a specific bug) goes
next to the `.c`/`.cpp` code. Long-term architectural decisions go in an ADR or design
document, referenced from the header/implementation.

## Test Coverage Criteria

ZLink's default target coverage is **80% line coverage**. This number is only a
baseline — it doesn't substitute for judgment:

- Coverage should prioritize the public contract, protocol compatibility, lifecycle
  boundaries, error paths, timeout/abort behavior, backpressure, and sample regression
  tests.
- A high coverage number doesn't prove quality if important user-visible behavior is
  missing.
- Falling below 80% needs a clear reason, like code better suited to integration/
  contract tests than line coverage — generated code, platform-specific glue code (this
  follows the condition in the "Testing" section of Part 1 of the general document).
- Don't add shallow tests just to move the number.

If a module exposes a public API or a cross-language contract, prefer focused contract
tests over tests that broadly pin down implementation detail. The goal is to uphold the
module's guarantees while keeping the implementation easy to refactor.

## Refactoring and Regression Testing

ZLink refactors against [POSDDD](./posddd.md) on an ongoing basis — not deferred to a
separately scheduled big cleanup.

**When and how.** The procedure is mechanical: once a related feature's development
passes its regression tests, sweep that change's scope right there against
[POSDDD](./posddd.md)'s risk sign checklist (information leakage, temporal
decomposition, shallow modules, and the other 19 items) and the shallow module smell
catalog. If any matching smell turns up, refactor it on the spot instead of deferring
it to "clean up later." A green
regression-test state is the point where it's safe to change the structure — changing
structure without confirming the tests pass first mixes functional defects with
refactoring defects and makes the cause hard to isolate.

This review also targets the core, bindings, and framework boundaries independently —
a refactor at one boundary must not touch another boundary's public contract (see
"Core, Bindings, Framework" above).

**Regression testing is not optional.** Whether it's a refactor or a regular feature
change, always run the relevant regression tests before committing and confirm they
pass. For a change touching the public contract, treat contract tests (public API
behavior, error codes, cross-language parity) as the baseline; for a change touching
only internal structure, treat the existing suite as the baseline. A refactor that
hasn't been confirmed by tests is "code that looks better," not "a verified
improvement."

## ZLink-Specific Risk Sign Checklist

In addition to the general document's 19-item checklist, check the following.

| # | Risk sign | Diagnostic question |
|---|---|---|
| Z1 | **Comment in the wrong place** | Is the public contract (ownership, error conditions, the meaning of timeout) documented only in the implementation, not the header? |
| Z2 | **Cross-language name mismatch** | Does the same system concept use a different name or meaning across language bindings? |
| Z3 | **Public API spread with no spec** | Did a public API or behavior that existed in only one language propagate to other languages with no spec or guide-document grounds? (A sign of an `AGENTS.md` violation) |
| Z4 | **Meaning drift across a boundary** | Does a word like timeout, cancellation, backpressure, or ownership get interpreted differently crossing a transport/codec/storage boundary? |
| Z5 | **Domain rules inside Infrastructure** | Does business-rule logic live directly inside ZLink handler/Spot/Actor callback code (`Infrastructure/ZLink/`)? |

---

> Judgments not covered by this document follow [POSDDD](./posddd.md) — what this
> document covers is ZLink's own facts and policies, which POSDDD doesn't.
