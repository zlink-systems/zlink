---
title: "Per-Language Binding Guides"
---

<!-- bindings-nav:start -->
[Next: .NET](dotnet/index.en.md)
<!-- bindings-nav:end -->

# Per-Language Binding Guides

> **Contract-owning document for this chapter** — each language's
> [binding spec](../spec/README.en.md) covers the public contract. This chapter is
> the entry point for picking a language and jumping to its guide.

zlink provides bindings in several languages on top of a C core. Each guide covers
**how to use zlink in that language** — installation, idiomatic examples, type
mapping, language-specific conventions. The messaging **concepts themselves**
(socket patterns, transport, services, routing IDs) are language-neutral and are
covered once, in the [core guide](https://kairos-code-dev.github.io/zlink/guide/01-overview/);
each language guide links back to the core guide wherever a concept is needed.

## Reading Order

- **You already know messaging / want to get moving fast** → go straight to your
  language's guide. Follow the core links inline whenever a concept is unclear.
- **Messaging is new to you** → read the core [overview](https://kairos-code-dev.github.io/zlink/guide/01-overview/)
  and [socket patterns](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/)
  first, then come to the language guide.

## Choosing A Language

| Language | Usage guide | Generated API reference | Package |
|------|------------|------------------|--------|
| .NET | [dotnet/](dotnet/index.en.md) | docfx | `Systems.Zlink` |
| C++ | [cpp/](cpp/index.en.md) | doxygen | `zlink::cpp` (CMake) |
| Java | [java/](java/index.en.md) | javadoc | `systems.zlink:zlink` |
| Node | [node/](node/index.en.md) | typedoc | `@zlink-systems/zlink` |
| Python | [python/](python/index.en.md) | sphinx | `zlink` (PyPI) |
| Go | [go/](go/index.en.md) | godoc | `zlink.systems/zlink/v11` |
| Rust | [rust/](rust/index.en.md) | rustdoc | `zlink` (crates.io) |
| Kotlin | [java/ §Kotlin](java/index.en.md#kotlin) | (shared with java) | `systems.zlink:zlink` |
| JavaScript | [node/ §JavaScript](node/index.en.md#javascript) | (shared with node) | `@zlink-systems/zlink` |

> Kotlin and JavaScript **share a runtime** — Kotlin uses the Java binding
> (`systems.zlink.*`) as-is, and JavaScript uses the Node binding
> (`@zlink-systems/zlink`) as-is. Since there's no separate native binding, each is
> covered in a dedicated section of the Java/Node guide. The core guide's language
> tabs also have separate Kotlin/JavaScript columns.

> C is the core itself, so instead of a separate binding guide, see the
> [core C API guide](https://kairos-code-dev.github.io/zlink/guide/02-core-api/).

## Guide Structure (common to every language)

Each language guide is a **single `index` document** that covers only that
language's surface — installation, types, ownership, mapping table, deployment.
Messaging, service, and operations concepts are covered once in the core guide,
and the language guide's "See Also" section links out to those core documents.

| index section | Content |
|------|------|
| Installation | Package install · namespace |
| 5-minute example | First PING/ACK round trip |
| Core types | Context · Message · Received · RoutingId |
| Ownership and lifetime | Resource-release rules |
| Error handling | Per-operation exception/error types |
| C API mapping | C API ↔ language surface mapping |
| Native library / deployment | Bundling · load path · publishing |
| Samples | Sample project pointers |
| See Also | Links to the core guide's socket-pattern/service/operations chapters |

> Per-socket-pattern usage, SPOT/Actor, options/TLS/monitoring/poller/timer,
> threading, and the like are now covered in the core guide, with per-language
> example tabs.
