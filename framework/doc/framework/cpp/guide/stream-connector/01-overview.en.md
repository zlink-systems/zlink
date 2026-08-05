# 01 — Overview

[← Table Of Contents](INDEX.en.md) | [Next: Getting Started →](02-getting-started.en.md)

---

The C++ Stream Connector is a client-side library that connects to a ZLink STREAM server. It's split
into a product family so the same STREAM protocol can be used across a variety of environments —
game engines, general C++ applications, server e2e tests, and more.

## Product Family Composition

```
connector/
├── core/          — connector runtime for general C++ clients (no-exception, no-coroutine)
├── e2e-client/    — coroutine helper for server e2e/perf scenarios
├── engines/       — Unreal, Godot, Axmol engine adapters
└── perf/          — performance test client and runner
```

Each deliverable is distributed independently. You can use only the engine adapter without
installing core, and core works fine without using the e2e client.

## Deployment Units

| Artifact | CMake target | Distribution format | Main users |
|--------|-------------|-----------|-------------|
| `zlink-stream-connector` | `zlink::stream_connector` | CMake, vcpkg, Conan | General C++ clients, inside game engines |
| `zlink-stream-e2e-client` | `zlink::stream_e2e_client` | CMake, vcpkg, Conan | Server e2e/smoke/perf tests |
| `zlink-unreal-stream-connector` | Unreal plugin module | source plugin | Unreal Engine games |
| `zlink-godot-stream-connector` | GDExtension | source GDExtension | Godot 4 games |
| `zlink-axmol-connector` | CMake target | source package | Axmol engine games |

## core — The Basic Connector

core is a standalone library that doesn't depend on C++ exceptions or coroutines. It builds even in
a game engine with exceptions turned off. The public header doesn't expose `<coroutine>` or a
Boost.Asio executor type.

```cpp
#include <zlink/stream_connector.hpp>

zlink::stream_connector::connector_options_t options;
options.endpoint = "tcp://game.example.com:7000";
auto connector = zlink::stream_connector::connector_factory_t::create(options);
```

Failures are returned as `result_t<T>`.

## e2e-client — The Coroutine Helper

The e2e client is an optional surface layered on top of core. Use it in an environment where C++20
coroutines can be reliably turned on. It's not the default API for a general game client.

```cpp
#include <zlink/stream_e2e_client.hpp>

auto client = zlink::stream_e2e_client::use(connector);
auto reply = co_await client.request(ping_t{"player-1", 1}).async<pong_t>();
```

`async()` doesn't call the blocking `submit()`. While the coroutine waits, the worker thread handles
other work.

## Supported Engines

| Engine | Support | Adapter Format |
|------|------|-------------|
| Unreal Engine | supported | `.uplugin` + `UObject` API + Game Thread delegate |
| Godot 4 | supported | GDExtension + Godot signal |
| Axmol Engine | supported | C++ source package + `Scheduler::runOnAxmolThread` |
| Cocos Creator 3.x | not supported in C++ | uses the TypeScript connector |
| Cocos2d-x | not supported | updates discontinued |

## Transport Support

| scheme | transport | build feature |
|--------|-----------|---------------|
| `tcp://host:port` | TCP | always included |
| `tls://host:port` | TLS over TCP | `WITH_TLS` (OpenSSL) |
| `ws://host:port/path` | WebSocket | `WITH_WEBSOCKET` |
| `wss://host:port/path` | WebSocket over TLS | `WITH_WEBSOCKET` + `WITH_TLS` |

## Relationship With The Server Framework

The connector is a client library that connects to a STREAM server. It has no mutual dependency with
the server framework package. The two sides only share the STREAM header/payload wire contract.
