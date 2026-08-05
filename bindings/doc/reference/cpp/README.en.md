[한국어](README.ko.md) | English

[C++ binding spec](../../spec/cpp/README.en.md) · [C++ binding guide](../../guide/cpp/index.en.md)

# C++ bindings reference

The writing rules follow the
[Reference-writing guide](../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This is the bindings layer (the Core C ABI's language projection) — not the
framework layer, which has its own reference tree under `framework/doc/framework/cpp/reference/`.

Categories follow the [.NET binding spec](../dotnet/README.en.md)'s Contract Folder Layout as the
common architecture map (the [C++ binding spec](../../spec/cpp/README.en.md#contract-folder-layout)
projects it onto C++ naming, not a copy of the C# shape). As with dotnet's reference tree, this
tree has five categories, not six — `include/zlink/Contracts/` has no `Service/` folder; SPOT/Actor
exists only at the framework layer. Also as with dotnet, `Contracts/`'s prose lists files this
directory listing doesn't have; the Contract-source column below is verified against `find`, not
copied from spec prose.

Two C++-specific placement notes carried into every category below:

- **Two header trees exist.** `include/zlink/{core,eventing,message,socket}/api.h` are `extern "C"`
  declarations — the low-level part-substrate layer for bindings implementers, not the public C++
  contract. The public contract is `include/zlink/Contracts/<Category>/*.hpp`, aggregated by the
  single entry header `<zlink.hpp>`.
- **`proxy`/`proxy_steerable` live in the Sockets category**, not Core — they are free functions
  taking `socket_t&` (`Contracts/Sockets/socket_contracts.hpp`), unlike dotnet's `Zlink` static
  facade placement. There is no C++ equivalent of dotnet's `Zlink.Sleep(...)`/
  `Zlink.MultipartClose(...)` free-function convenience helpers today.

## Locale convention

Every `bindings/doc/spec/<lang>/` document is English-original, Korean-translation (unlike the
framework's interface-catalog convention). This reference tree follows the same direction: write
`.en.md` first, `.ko.md` second, and every spec citation links to the same-locale spec file.

## Category

| Category | Status | Contract source (verified against `include/zlink/Contracts/`) |
|---|---|---|
| [Core](01-core.en.md) | Drafted | `Contracts/Core/`: `context.hpp`, `context_options.hpp`, `routing_id.hpp`, `byte_count.hpp`, `capability.hpp`, `utilities.hpp` |
| [Messaging](02-messaging.en.md) | Drafted | `Contracts/Messaging/`: `message.hpp`, `received.hpp`, `topic_message.hpp`, `subscription_event.hpp`, `operation_contracts.hpp`, `request_result.hpp` (`lazy_message_parts.hpp` and `operation_builder_base.hpp` are `detail`, no public entry) |
| [Sockets](03-sockets.en.md) | Drafted | `Contracts/Sockets/`: `socket_contracts.hpp`, `message_socket_contracts.hpp`, `routed_socket_contracts.hpp`, `pubsub_socket_contracts.hpp`, `stream_socket.hpp`, `socket_options.hpp`, `results.hpp` |
| [Eventing](04-eventing.en.md) | Drafted | `Contracts/Eventing/`: `monitor.hpp`, `status.hpp`, `poller.hpp`, `poll_event.hpp`, `timers.hpp`, `events.hpp` |
| [Errors](05-errors.en.md) | Drafted | `Contracts/Errors/`: `errors.hpp`, `results.hpp` |

This document tree is wired into `mkdocs.yml` nav.
