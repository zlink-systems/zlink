[한국어](README.ko.md) | English

[Rust binding spec](../../spec/rust/README.en.md) · [Rust binding guide](../../guide/rust/index.en.md)

# Rust bindings reference

The writing rules follow the
[Reference-writing guide](../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This is the bindings layer (the Core C ABI's language projection) — not the
framework layer, which has its own reference tree under `framework/doc/framework/rust/reference/`.

Categories follow the [.NET binding spec](../dotnet/README.en.md)'s Contract Folder Layout as the
common architecture map. As with every wrapper binding so far, this tree has five categories, not
six — `src/contracts/` has no `service/` module; SPOT/Actor exists only at the framework layer. The
Contract-source column below is verified against the actual file listing, not copied from spec
prose.

Rust-specific notes carried into every category below:

- **The factory/utility free functions (`version`, `has`, `proxy`, `sleep`, `poll`, ...) are not
  declared under `contracts/core/` at all** — they are plain functions at the crate root
  (`bindings/rust/src/lib.rs`), the idiomatic Rust equivalent of node's package-root export style,
  rather than a static facade type (dotnet's `Zlink`, java's `Zlink`) or free functions in a
  dedicated namespace (cpp's `zlink::`).
- **There is no shared cross-socket-type base trait.** Every concrete socket is a standalone struct
  with its own inherent `impl` block; `bind`/`connect`/`unbind`/`disconnect`/TLS methods are
  redeclared independently (or via an internal macro for the four PUB/SUB/XPUB/XSUB types) rather
  than inherited from a shared `Socket`/`ConnectableSocket` trait the way every other language
  covered so far provides. `Pollable`/`Monitorable` are the only cross-cutting traits, and both are
  `sealed` — a crate consumer cannot implement either for a custom type.
- **`ZlinkError` is a Rust enum wrapping each typed error variant** (`Submit(SubmitError)`,
  `Request(RequestError)`, ...), not an inheritance base class — the idiomatic Rust shape for "one
  of several typed errors."
- **No async/Future-returning request submit exists in this binding's public contract** — unlike
  every other language covered so far (dotnet's `Task`, java's `CompletionStage`, node's `Promise`,
  cpp's `async_result_t`), `RequestOp::submit` is callback-only.

## Locale convention

Every `bindings/doc/spec/<lang>/` document is English-original, Korean-translation (unlike the
framework's interface-catalog convention). This reference tree follows the same direction: write
`.en.md` first, `.ko.md` second, and every spec citation links to the same-locale spec file.

## Category

| Category | Status | Contract source (verified against `src/contracts/` + `src/lib.rs`) |
|---|---|---|
| [Core](01-core.en.md) | Drafted | `src/lib.rs` (free functions); `contracts/core/`: `context.rs`, `routing_id.rs`, `utilities.rs` |
| [Messaging](02-messaging.en.md) | Drafted | `contracts/messaging/`: `message.rs`, `received.rs`, `topic_message.rs`, `subscription_event.rs`, `operation_contracts.rs`, `operations.rs` |
| [Sockets](03-sockets.en.md) | Drafted | `contracts/sockets/`: `socket.rs`, `message_socket_contracts.rs`, `routed_socket_contracts.rs`, `pubsub_socket_contracts.rs`, `stream_socket.rs`, `socket_options.rs` |
| [Eventing](04-eventing.en.md) | Drafted | `contracts/eventing/`: `poller.rs` (also owns `Timer`), `monitor.rs` |
| [Errors](05-errors.en.md) | Drafted | `contracts/errors/`: `errors.rs`, `results.rs` |

This document tree is wired into `mkdocs.yml` nav.
