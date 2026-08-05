[한국어](README.ko.md) | English

[Node binding spec](../../spec/node/README.en.md) · [Node binding guide](../../guide/node/index.en.md)

# Node bindings reference

The writing rules follow the
[Reference-writing guide](../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This is the bindings layer (the Core C ABI's language projection) — not the
framework layer, which has its own reference tree under `framework/doc/framework/node/reference/`.
JavaScript (plain, non-TypeScript) shares this same bindings runtime via a symlink (no separate
`bindings/javascript/` contract source exists — only `bindings/javascript/samples/`), so this tree
also serves as JavaScript's bindings-layer reference.

Categories follow the [.NET binding spec](../dotnet/README.en.md)'s Contract Folder Layout as the
common architecture map. As with every wrapper binding so far, this tree has five categories, not
six — `zlink/contracts/` has no `service/` folder; SPOT/Actor exists only at the framework layer.
The Contract-source column below is verified against the actual file listing, not copied from spec
prose.

One Node-specific note carried into the Core category below: **the factory functions
(`createContext`, `createPairSocket`, ..., `version`, `has`, `proxy`, `sleep`, ...) are not declared
under `contracts/core/` at all** — they are top-level functions exported from the package root
(`bindings/node/src/index.ts`), following the Node/JS idiom of function-based module exports rather
than a static class facade (dotnet's `Zlink`, java's `Zlink`, cpp's free functions under
`zlink::`). `contracts/core/` itself holds only the `Context`/`ContextOptions` interfaces and the
`RoutingId` class.

## Locale convention

Every `bindings/doc/spec/<lang>/` document is English-original, Korean-translation (unlike the
framework's interface-catalog convention). This reference tree follows the same direction: write
`.en.md` first, `.ko.md` second, and every spec citation links to the same-locale spec file.

## Category

| Category | Status | Contract source (verified against `zlink/contracts/` + `src/index.ts`) |
|---|---|---|
| [Core](01-core.en.md) | Drafted | `src/index.ts` (factory functions + `version`/`has`/`proxy`/`sleep`/`multipartClose`); `contracts/core/`: `context.ts`, `routing_id.ts`, `buffer_like.ts` |
| [Messaging](02-messaging.en.md) | Drafted | `contracts/messaging/`: `message.ts`, `received.ts`, `topic_message.ts`, `subscription_event.ts`, `operations.ts`, `handlers.ts`, `message_parts_envelope.ts` |
| [Sockets](03-sockets.en.md) | Drafted | `contracts/sockets/`: `socket.ts`, `pair_socket.ts`, `dealer_socket.ts`, `router_socket.ts`, `pubsub_sockets.ts`, `stream_socket.ts`, `socket_options.ts`, `socket_constants.ts` |
| [Eventing](04-eventing.en.md) | Drafted | `contracts/eventing/`: `monitor.ts`, `poller.ts`, `timer.ts` |
| [Errors](05-errors.en.md) | Drafted | `contracts/errors/`: `errors.ts`, `results.ts` |

This document tree is wired into `mkdocs.yml` nav.
