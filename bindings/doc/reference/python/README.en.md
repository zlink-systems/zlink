[한국어](README.ko.md) | English

[Python binding spec](../../spec/python/README.en.md) · [Python binding guide](../../guide/python/index.en.md)

# Python bindings reference

The writing rules follow the
[Reference-writing guide](../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This is the bindings layer (the Core C ABI's language projection) — not the
framework layer, which has its own reference tree under `framework/doc/framework/python/reference/`.

Categories follow the [.NET binding spec](../../spec/dotnet/README.en.md)'s Contract Folder Layout as the
common architecture map. As with every wrapper binding so far, this tree has five categories, not
six — `contracts/` has no `service/` package; SPOT/Actor exists only at the framework layer. The
Contract-source column below is verified against the actual file listing, not copied from spec
prose.

Python-specific notes carried into every category below:

- Sockets and option facades are `typing.Protocol` types. Event snapshots and enums
  are concrete types.
- Import factories such as `create_context` and `create_pair_socket` from the package root.
- Sockets support `with`. `Context` and `Message` support both `with` and `async with`.
- DEALER, ROUTER, and STREAM expose `set_routing_id` and `get_routing_id` for the local
  socket identity. `RouterSocketOptions.connect_routing_id` sets the peer routing ID
  for the next outbound connection.

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
