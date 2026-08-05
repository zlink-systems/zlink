<!-- framework-adapter-nav:start -->
[Document list](README.en.md) | [Next: ZLink Framework Common Spec](framework/common/README.en.md)
<!-- framework-adapter-nav:end -->

# ZLink Framework Document Map

The entry point for the `ZLink Framework` documentation. Documents are split **per component**,
and each component splits into **common (language-neutral)** and **per-language** documents. Jump
straight to the location you need from the map below.

## Components At A Glance

| Component | Directory | Entry Point | Content |
|---------|----------|--------|------|
| **Framework adapter** | `framework/` | [Common spec](framework/common/README.en.md) | The core messaging/SPOT/actor/stream framework |
| **HTTP Client** | `framework/<lang>/guide/http-client/` | [Per-language docs](framework/dotnet/guide/http-client/README.en.md) | Fluent HTTP/JSON client |
| **Stream Connector** | `framework/<lang>/guide/stream-connector/` | [Per-language docs](framework/cpp/guide/stream-connector/README.en.md) | Client-side STREAM connection library |

Official languages: `.NET` · `Java/Kotlin` · `Node.js` · `C++`.

---

## 1. Framework Adapter (`framework/`)

### 1.1 Common — Language-Neutral Formal Contract (`framework/common/`)

[Common document index](framework/common/README.en.md) · Fix the common meaning here first.

**Spec** (`framework/common/spec/` — split per package: `server/` · `http-client/` ·
`stream-connector/`)

| Document | Scope |
|------|-------------|
| [Overview](framework/common/spec/02-overview.en.md) | The Framework's purpose and priority scope |
| [Interaction model](framework/common/spec/03-interaction-model.en.md) | The request-response, command, publish-subscribe user model |
| [Message model](framework/common/spec/04-message-model.en.md) | The header/payload structure and metadata policy |
| [Channel topology](framework/common/spec/07-channel-topology.en.md) | Channel grouping, discovery, manual connection, internal transport mapping |
| [Framework API](framework/common/spec/06-framework-api.en.md) | The common direction of the per-language framework API |
| [Async execution policy](framework/common/spec/05-async-execution-policy.en.md) | Async submit, the ban on blocking, coroutine/adapter common meaning |
| [Actor model](framework/common/spec/14-actor-model.en.md) | Actor location, session binding, Entry Spot, user Spot, dispatch criteria |
| [Session Actor Dispatch](framework/common/spec/20-session-actor-dispatch.en.md) | The helper and routing policy that connects a session to an actor |

**Common sample scenarios** ([index](framework/common/sample/README.en.md)) — every language
implements the 6 canonical samples with the same role separation, message names, and smoke order.

The Sample and E2E configuration files, the ban on environment variables, and the per-language
typed binding criteria follow the
[Sample/E2E Configuration Policy](framework/common/sample-e2e-configuration-policy.en.md).

| Sample | Core |
|------|------|
| TicTacToe | Route mesh, actor game join (direct API+Play connection) |
| Bingo | Registry/Discovery, Entry Spot, room Spot, bound push (Session/API/Play separated) |
| SupportChat | Conversation Spot, idle/close timer, reconnect |
| DeliveryDispatch | Delivery status transitions, reassignment timer, customer stream push |
| ShoppingMall | Order workflow state transitions, event sourcing/compensation |
| GameQuest | Stateless API scale-out, owner routing, fanout + event sourcing |

### 1.2 Per-Language (`framework/<lang>/`, Rooted At `framework/doc`)

Each language's documents are managed under this directory, `framework/doc/`. They split into
`guide/server`, `guide/http-client`, `guide/stream-connector` (usage guides) and `internals`
(implementation/verification criteria), and each language's README indexes that entire language.
The public contract is managed per package and per language under `framework/common/spec/`.

| Language | Entry Point | Guide | Spec | Internals |
|------|--------|-------|------|-----------|
| `.NET` | [framework/dotnet](framework/dotnet/README.en.md) | Feature guide + samples | ASP.NET Core contract | Backend/runtime/regression |
| `Java` | [framework/java](framework/java/README.en.md) | Spring Boot guide (blocking/`CompletionStage`) | Spring contract | Implementation/verification |
| `Kotlin` | [framework/kotlin](framework/kotlin/README.en.md) | Kotlin-only guide (`suspend`/`Flow`) | Shared with Java | Shared with Java |
| `Node.js` | [framework/node](framework/node/README.en.md) | NestJS guide | NestJS contract | Implementation/verification |
| `C++` | [framework/cpp](framework/cpp/README.en.md) | Guide + samples | C++ contract | Runtime/backend/regression |

> Guide skeleton (per-language chapter numbers can differ): overview → getting started → core
> concepts → channel messaging → SPOT → actor/session → STREAM → Registry → monitoring →
> interface catalog → gRPC alternative → choosing a sample, in that order.

---

## 2. HTTP Client (`framework/<lang>/guide/http-client/`)

A fluent HTTP/JSON client. All five languages share the same 13-chapter skeleton. Kotlin provides
the same functionality through a coroutine `suspend` surface.

| Language | Entry Point |
|------|--------|
| `.NET` | [framework/dotnet/guide/http-client](framework/dotnet/guide/http-client/README.en.md) |
| `Java` | [framework/java/guide/http-client](framework/java/guide/http-client/README.en.md) |
| `Kotlin` | [framework/kotlin/guide/http-client](framework/kotlin/guide/http-client/README.en.md) |
| `Node.js` | [framework/node/guide/http-client](framework/node/guide/http-client/README.en.md) |
| `C++` | [framework/cpp/guide/http-client](framework/cpp/guide/http-client/README.en.md) |

Chapter structure: 01 overview · 02 getting started · 03 client configuration · 04 sending
requests · 05 request body · 06 handling the response · 07 async/coroutine · 08 streaming · 09
authentication/TLS · 10 redirect/retry/cookie · 11 proxy · 12 compression · 13 error handling. The
public contract lives in each language's `spec/`.

---

## 3. Stream Connector (`framework/<lang>/guide/stream-connector/`)

A separate library for connecting to a STREAM server from the client side. Includes game engine
adapters.

**Which connector to use is decided by "engine × build target," not language.** If you build for
the web (browser/WASM), you use the TypeScript connector regardless of language.

| Language | Target | Guide |
|------|------|--------|
| C++ | Unreal, Godot (GDExtension), Axmol, plain C++, server e2e/perf | [INDEX](framework/cpp/guide/stream-connector/INDEX.en.md) |
| `.NET` | Unity (native), Godot C#, desktop/server | [INDEX](framework/dotnet/guide/stream-connector/INDEX.en.md) |
| Java | JVM applications/tools/E2E clients/bots | [Guide](framework/java/guide/stream-connector/README.en.md) |
| Kotlin | JVM applications/tools/E2E clients/bots | [Guide](framework/kotlin/guide/stream-connector/README.en.md) |
| Node.js/TypeScript | Browser-family (web, Unity WebGL, Cocos web, Godot Web) | [INDEX](framework/node/guide/stream-connector/INDEX.en.md) |

| Area | Documents |
|------|------|
| C++ guide | [01 Overview](framework/cpp/guide/stream-connector/01-overview.en.md) · [02 Getting Started](framework/cpp/guide/stream-connector/02-getting-started.en.md) · [03 Options](framework/cpp/guide/stream-connector/03-connector-options.en.md) · [04 Sending](framework/cpp/guide/stream-connector/04-sending.en.md) · [05 Receiving](framework/cpp/guide/stream-connector/05-receiving.en.md) · [06 Lifecycle](framework/cpp/guide/stream-connector/06-lifecycle.en.md) · [07 Error Handling](framework/cpp/guide/stream-connector/07-error-handling.en.md) · [08 E2E Client](framework/cpp/guide/stream-connector/08-e2e-client.en.md) · [09 Engine Adapters](framework/cpp/guide/stream-connector/09-engine-adapters.en.md) · [10 Packaging](framework/cpp/guide/stream-connector/10-packaging.en.md) · [11 Performance](framework/cpp/guide/stream-connector/11-performance.en.md) |
| C++ core | [Async runtime](framework/cpp/guide/stream-connector/core/guide/async-runtime.en.md) |
| C++ e2e-client | [Coroutine client](framework/cpp/guide/stream-connector/e2e-client/guide/coroutine-client.en.md) |
| `.NET` guide | [01 Overview](framework/dotnet/guide/stream-connector/01-overview.en.md) · [02 Unity](framework/dotnet/guide/stream-connector/02-unity.en.md) · [03 Godot C#](framework/dotnet/guide/stream-connector/03-godot-csharp.en.md) |
| Node.js/TypeScript guide | [01 Overview](framework/node/guide/stream-connector/01-overview.en.md) · [02 Browser](framework/node/guide/stream-connector/02-browser.en.md) |

The formal contract is the
[Stream Connector common spec](framework/common/spec/stream-connector/32-stream-connector.en.md),
and the per-language public surface is owned by
`framework/common/spec/stream-connector/languages/<lang>/`.

Java and Kotlin use the same JVM connector runtime. The Java guide explains `CompletionStage`
usage, and the Kotlin guide explains coroutine and `Flow` usage.

> The TypeScript package root provides browser-only WebSocket transport and explicit flow
> delivery. Check the actual browser and package verification status in the TypeScript package's
> test and E2E results.

---

## Reading Order

1. [Common overview](framework/common/spec/02-overview.en.md) → [Interaction model](framework/common/spec/03-interaction-model.en.md) → [Actor model](framework/common/spec/14-actor-model.en.md)
2. The [framework/&lt;lang&gt;](framework/dotnet/README.en.md) guide for the language you'll use
3. If you need HTTP, [framework/&lt;lang&gt;/guide/http-client](framework/dotnet/guide/http-client/README.en.md); for external client connections, [framework/&lt;lang&gt;/guide/stream-connector](framework/cpp/guide/stream-connector/INDEX.en.md)

## Maintenance Rules

- Fix common meaning in `framework/common/spec/` first, and link the language documents to it.
- Per-language documents only make the common meaning concrete with that language's signatures and
  samples.
- Every per-language document is written and edited under `framework/doc/`. Don't add a new
  per-language document under `framework/languages/<lang>/doc/`.
- If an existing per-language usage guide needs editing, do it under the matching location among
  `server/`, `http-client/`, `stream-connector/` in `framework/doc/framework/<lang>/guide/`.
  Implementation explanations are managed in `framework/doc/framework/<lang>/internals/`.
- When adding a new document, update this map and that directory's `README.ko.md` together.
- Keep the component boundary: the framework core is `framework/`, the HTTP client is
  `http-client/`, and the STREAM connector is `stream-connector/`.

---
<!-- framework-adapter-nav:bottom:start -->
[Document list](README.en.md) | [Next: ZLink Framework Common Spec](framework/common/README.en.md)
<!-- framework-adapter-nav:bottom:end -->
