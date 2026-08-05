[한국어](README.ko.md) | English

[Python binding spec](../../spec/python/README.en.md) · [Python binding guide](../../guide/python/index.en.md)

# Python bindings reference

The writing rules follow the
[Reference-writing guide](../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This is the bindings layer (the Core C ABI's language projection) — not the
framework layer, which has its own reference tree under `framework/doc/framework/python/reference/`.

Categories follow the [.NET binding spec](../dotnet/README.en.md)'s Contract Folder Layout as the
common architecture map. As with every wrapper binding so far, this tree has five categories, not
six — `contracts/` has no `service/` package; SPOT/Actor exists only at the framework layer. The
Contract-source column below is verified against the actual file listing, not copied from spec
prose.

Python-specific notes carried into every category below:

- **Every contract type is a `typing.Protocol`** (structural typing, most `@runtime_checkable`),
  not a concrete base class — the actual implementations live under `_runtime`/`_native` and are
  never imported directly; a caller only ever sees the `Protocol` shape.
- **Factory functions live at the package root** (`bindings/python/src/zlink/__init__.py`:
  `create_context`, `create_pair_socket`, `version`, `has`, `proxy`, `sleep`, ...), the same
  package-root idiom as node/rust, not `contracts/core/`.
- **Every resource type that owns native storage supports both sync and async context-manager
  protocols** (`__enter__`/`__exit__` *and* `__aenter__`/`__aexit__`) — `with`/`async with` both
  work, unique among every language covered so far.
- **No socket type in this binding declares `set_routing_id`/`get_routing_id`/a `routing_id`
  property** — not `DealerSocket`, not `RouterSocket`, not `StreamSocket`. Only
  `RouterSocketOptions.connect_routing_id` exists, and that assigns the routing id for the *next
  outgoing connection*, not the socket's own identity. Every other language covered so far exposes
  a routing-id setter/getter on at least Dealer/Router/Stream.

## Locale convention

Every `bindings/doc/spec/<lang>/` document is English-original, Korean-translation (unlike the
framework's interface-catalog convention). This reference tree follows the same direction: write
`.en.md` first, `.ko.md` second, and every spec citation links to the same-locale spec file.

## Category

| Category | Status | Contract source (verified against `contracts/` + `__init__.py`) |
|---|---|---|
| [Core](01-core.en.md) | Drafted | `__init__.py` (factory/free functions); `contracts/core/`: `context.py`, `options.py`, `routing_id.py`, `utilities.py`, `codes.py` |
| [Messaging](02-messaging.en.md) | Drafted | `contracts/messaging/`: `message.py`, `received.py`, `topic_message.py`, `subscription_event.py` (no separate `operations.py` — the builder Protocols live in `contracts/sockets/operations.py`) |
| [Sockets](03-sockets.en.md) | Drafted | `contracts/sockets/`: `socket.py`, `message_socket_contracts.py`, `routed_socket_contracts.py`, `pubsub_socket_contracts.py`, `stream_socket.py`, `socket_options.py`, `operations.py`, `codes.py` |
| [Eventing](04-eventing.en.md) | Drafted | `contracts/eventing/`: `poller.py`, `monitor.py`, `timer.py`, `codes.py` |
| [Errors](05-errors.en.md) | Drafted | `contracts/errors/`: `errors.py`, `results.py`, `codes.py` |

This document tree is wired into `mkdocs.yml` nav.
