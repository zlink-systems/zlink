[English](./README.md) | [한국어](./README.ko.md)

# zlink

> A modern messaging library built on [libzmq](https://github.com/zeromq/libzmq) v4.3.5 — focused on the essential patterns, with Boost.Asio-powered I/O and a developer-friendly API.

[![Build](https://github.com/ulala-x/zlink/actions/workflows/build.yml/badge.svg)](https://github.com/ulala-x/zlink/actions/workflows/build.yml)
[![License: MPL-2.0 / FSL-1.1 / Apache-2.0](https://img.shields.io/badge/License-multiple-blue.svg)](./doc/license/README.md)

[Website](https://kairos-code-dev.github.io/zlink/) · [User Guide](./doc/guide/01-overview.md) · [Spec](./doc/spec/README.md) · [Bindings](doc/bindings/overview.md) · [Internals](./doc/internals/architecture.md) · [Build](./doc/building/build-guide.md)

---

## Why zlink?

libzmq is powerful, but it carries decades of accumulated complexity — legacy protocols, rarely used socket types, and an I/O engine designed for a different era.

**zlink strips libzmq down to its core and rebuilds it for the modern world:**

| | libzmq | zlink |
|---|--------|-------|
| **Socket Types** | 17 (incl. draft) | **7** — PAIR, PUB/SUB, XPUB/XSUB, DEALER/ROUTER, STREAM |
| **I/O Engine** | Custom poll/epoll/kqueue | **Boost.Asio** (bundled, no external dependencies) |
| **Encryption** | CURVE (libsodium) | **TLS** (OpenSSL) — `tls://`, `wss://` |
| **Transport** | 10+ (PGM, TIPC, VMCI, etc.) | **6** — `tcp`, `ipc`, `inproc`, `ws`, `wss`, `tls` |
| **Dependencies** | libsodium, libbsd, etc. | **OpenSSL only** |

---

## Key Features

### Streamlined Core

REQ/REP, PUSH/PULL, and all draft sockets have been removed. The remaining 7 socket types — PAIR, PUB/SUB, XPUB/XSUB, DEALER/ROUTER, STREAM — cover the vast majority of real-world messaging patterns while reducing mistakes caused by unnecessary complexity. The STREAM socket supports tcp, tls, ws, and wss transports for raw communication with external clients.
Current STREAM defaults are tuned for production throughput: accept concurrency `4`, session scheduling `rr`, and socket `SNDBUF/RCVBUF` auto-default to `256KB` when unset. Most legacy STREAM runtime tuning env flags were removed and replaced with fixed internal constants.

### Boost.Asio-Based I/O Engine

The entire I/O layer has been rewritten with **Boost.Asio** (header-only bundle — no external Boost dependency). TLS and WebSocket transports are natively integrated on top of a proven asynchronous foundation.

### Native TLS & WebSocket

Encrypted transports are supported directly, with no external proxy required:

```c
// TLS server
zlink_setsockopt(socket, ZLINK_TLS_CERT, "/path/to/cert.pem", ...);
zlink_setsockopt(socket, ZLINK_TLS_KEY, "/path/to/key.pem", ...);
zlink_bind(socket, "tls://*:5555");

// TLS client
zlink_setsockopt(socket, ZLINK_TLS_CA, "/path/to/ca.pem", ...);
zlink_connect(socket, "tls://server.example.com:5555");
```

---

## Architecture

zlink is composed of five clearly separated layers:

```
┌──────────────────────────────────────────────────────┐
│  Application Layer                                    │
│  zlink_ctx_new() · zlink_socket() · zlink_send/recv()      │
├──────────────────────────────────────────────────────┤
│  Socket Logic Layer                                   │
│  PAIR · PUB/SUB · XPUB/XSUB · DEALER/ROUTER · STREAM  │
│  Routing strategies: lb_t(Round-robin) · fq_t · dist_t │
├──────────────────────────────────────────────────────┤
│  Engine Layer (Boost.Asio)                            │
│  asio_zmp_engine — ZMP v1.0 Protocol (8B fixed header)│
│  Proactor pattern · Speculative I/O · Backpressure    │
├──────────────────────────────────────────────────────┤
│  Transport Layer                                      │
│  tcp · ipc · inproc · ws — plaintext                  │
│  tls · wss             — OpenSSL encrypted            │
├──────────────────────────────────────────────────────┤
│  Core Infrastructure                                  │
│  msg_t(64B fixed) · pipe_t(Lock-free YPipe)           │
│  ctx_t(I/O Thread Pool) · session_base_t(Bridge)      │
└──────────────────────────────────────────────────────┘
```

### Core Design

| Design Principle | Description |
|------------------|-------------|
| **Zero-Copy** | Minimizes message copying — VSM (33B or less) stored inline, larger messages use reference counting |
| **Lock-Free** | Inter-thread communication via YPipe (CAS-based FIFO), no mutexes |
| **True Async** | Proactor pattern-based async I/O with Speculative I/O optimization |
| **Protocol Agnostic** | Clean separation of Transport and Protocol — uses the custom ZMP v1.0 protocol |

### Threading Model

- **Application Thread**: Calls `zlink_send()`/`zlink_recv()`
- **I/O Thread**: Async network processing based on Boost.Asio `io_context`
- **Reaper Thread**: Cleans up resources from terminated sockets/sessions
- Inter-thread communication is handled through the Lock-free YPipe + Mailbox system

> For a detailed look at the internal architecture, see the [Architecture Document](./doc/internals/architecture.md).

---

## Services

Built on top of the core socket layer, zlink provides a **high-level service layer** for production distributed systems:

```
┌───────────────────────────────────────────────────────┐
│                      Application                      │
│          Gateway (req/rep) · SPOT (pub/sub)           │
├───────────────────────────────────────────────────────┤
│              Discovery (service lookup)               │
├───────────────────────────────────────────────────────┤
│              Registry (service registry)              │
├───────────────────────────────────────────────────────┤
│         zlink Core (7 sockets + 6 transports)         │
└───────────────────────────────────────────────────────┘
```

| Service | Description | Guide |
|---------|-------------|:-----:|
| **Discovery** | Registry cluster with HA, heartbeat-based health check, client-side service cache | [Discovery](./doc/guide/07-1-discovery.md) |
| **Gateway** | Location-transparent request/response with automatic load balancing, thread-safe send | [Gateway](doc/guide/07-2-gateway.md) |
| **SPOT** | Location-transparent topic PUB/SUB with Discovery-based automatic mesh | [SPOT](./doc/guide/07-3-spot.md) |

> For the full feature roadmap and dependency graph, see the [Feature Roadmap](doc/plan/feature-roadmap.md).

---

## Additional Features

| Feature | Description | Guide |
|---------|-------------|:-----:|
| **Routing ID** | `zlink_routing_id_t` standard type, own 16B UUID / peer 4B uint32 | [Routing ID](./doc/guide/08-routing-id.md) |
| **Monitoring** | Routing-ID-based event identification, polling-style monitor API | [Monitoring](./doc/guide/06-monitoring.md) |

---

## Getting Started

### Requirements

- **CMake** 3.10+
- **C++17** compiler (GCC 7+, Clang 5+, MSVC 2017+)
- **OpenSSL** (for TLS/WSS support)

### Build

```bash
# Linux
./core/builds/linux/build.sh x64 ON

# macOS
./core/builds/macos/build.sh arm64 ON

# Windows (PowerShell)
.\core\builds\windows\build.ps1 -Architecture x64 -RunTests "ON"
```

### Direct CMake Build

```bash
cmake -S core -B core/build/local -DWITH_TLS=ON -DBUILD_TESTS=ON
cmake --build core/build/local
ctest --test-dir core/build/local --output-on-failure
```

### C Perf Benchmark Build Rule

`bindings/c/perf/run_benchmarks_multi.sh` does not use `build_cpp_release`.
The standalone C benchmark build links zlink core from `core/build`, so benchmark
results are valid only after rebuilding that runtime.

```bash
cmake --build core/build
./bindings/c/perf/run_benchmarks_multi.sh --pattern SPOT_REQREP
```

The runner now prints the resolved `libzlink.so` path before execution and fails
fast when files under `core/src` or `core/include` are newer than the runtime
library in `core/build`.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `WITH_TLS` | `ON` | Enable TLS/WSS via OpenSSL |
| `BUILD_TESTS` | `ON` | Build tests |
| `BUILD_BENCHMARKS` | `OFF` | Build benchmarks |
| `BUILD_SHARED` | `ON` | Build shared library |
| `BUILD_STATIC` | `ON` | Build static library |
| `ZLINK_CXX_STANDARD` | `17` | C++ standard (11, 14, 17, 20, 23) |

### Installing OpenSSL

```bash
# Ubuntu/Debian
sudo apt-get install libssl-dev

# macOS
brew install openssl@3

# Windows (vcpkg)
vcpkg install openssl:x64-windows
```

---

## Supported Platforms

| Platform | Architecture | Status |
|----------|:------------:|:------:|
| Linux | x64, ARM64 | Stable |
| macOS | x64, ARM64 | Stable |
| Windows | x64, ARM64 | Stable |

---

## Performance

Throughput comparison with libzmq on 64-byte messages over TCP:

| Pattern | libzmq | zlink | Diff |
|---------|-------:|------:|-----:|
| DEALER↔DEALER | 5,936 Kmsg/s | 6,168 Kmsg/s | **+3.9%** |
| PAIR | 6,195 Kmsg/s | 5,878 Kmsg/s | -5.1% |
| PUB/SUB | 5,654 Kmsg/s | 5,756 Kmsg/s | **+1.8%** |
| DEALER↔ROUTER | 5,609 Kmsg/s | 5,634 Kmsg/s | +0.4% |
| ROUTER↔ROUTER | 5,161 Kmsg/s | 5,250 Kmsg/s | **+1.7%** |
| ROUTER↔ROUTER (poll) | 4,405 Kmsg/s | 5,249 Kmsg/s | **+19.2%** |
| STREAM | 1,786 Kmsg/s | 5,216 Kmsg/s | **+192%** |

> For detailed analysis, see the [Performance Report](doc/report/benchmark-2026-02-11.md) and the [Performance Guide](./doc/guide/10-performance.md).

---

## Documentation

| Document | Description |
|----------|-------------|
| [Documentation Index](./doc/README.md) | Full table of contents and reader-specific paths |
| [Library Spec](./doc/spec/README.md) | zlink library specification (core + bindings) |
| [User Guide](./doc/guide/01-overview.md) | zlink API guide (12 chapters) |
| [Bindings Guide](doc/bindings/overview.md) | C++/Java/.NET/Node.js/Python bindings |
| [Internal Architecture](./doc/internals/architecture.md) | System architecture and internals |
| [Build Guide](./doc/building/build-guide.md) | Building, testing, and packaging |
| [Feature Roadmap](doc/plan/feature-roadmap.md) | Feature roadmap and dependency graph |

---

## License

This repository uses three licenses, split by layer:

| Layer | License |
|-------|---------|
| `core/`, `bindings/` — the messaging engine and per-language native bindings | [Mozilla Public License 2.0](./LICENSE) |
| `framework/` — the higher-level framework (SPOT/actor, channel messaging, STREAM, drain) | [Functional Source License 1.1, ALv2 Future License](./framework/LICENSE) |
| `framework`'s `http-client` package in each language — a thin wrapper over that platform's commonly used HTTP client library (.NET's `System.Net.Http`, Java/Kotlin's `java.net.http`, Node's `undici`, C++'s Boost.Beast) | Apache License 2.0 |

Each `http-client` package wraps a permissively-licensed HTTP client library
rather than any zlink-original transport, so it carries Apache-2.0 instead of
FSL. The .NET and Node packages declare `Apache-2.0` directly in their own
package manifest; the Java and Kotlin packages receive it from the shared
Gradle publish configuration; the C++ package ships the license text
alongside its sources (CMake has no standard SPDX manifest field).

`core`/`bindings` stay MPL-2.0 because `core` began from
[libzmq](https://github.com/zeromq/libzmq) v4.3.5, itself MPL-2.0. `framework`
is licensed under FSL-1.1-ALv2: free for internal use, non-commercial
education/research, and professional services, plus anything else that
isn't a Competing Use — offering the Software as a substitute for the
Software itself, for another product or service we offer, or for anything
with substantially similar functionality (full definition in
[framework/LICENSE](./framework/LICENSE)). Building and shipping your own
product with it, including embedding it, falls under that last "isn't a
Competing Use" clause rather than the license's explicit list.
Each `framework` release automatically converts to Apache License 2.0 two
years after its publication date.

For third-party component notices and binary redistribution information,
see [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md). For the full policy
rationale, see [doc/license/README.md](./doc/license/README.md).

Built on [libzmq](https://github.com/zeromq/libzmq) — Copyright (c) 2007-2024 Contributors as noted in the [AUTHORS](./core/AUTHORS) file.
