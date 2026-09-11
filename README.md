**English** | [한국어](./README.ko.md)

# zlink

> A multi-language messaging platform that combines a core messaging engine,
> bindings for seven languages, and a real-time messaging framework implemented
> by four independent language-runtime families.

[![Build](https://github.com/zlink-systems/zlink/actions/workflows/build.yml/badge.svg)](https://github.com/zlink-systems/zlink/actions/workflows/build.yml)
[![License: MPL-2.0 / FSL-1.1 / Apache-2.0](https://img.shields.io/badge/License-multiple-blue.svg)](./doc/license/README.md)

[Website](https://zlink.systems/) ·
[Installation](https://zlink.systems/install/) ·
[Framework guide](https://zlink.systems/) ·
[Core guide](https://zlink.systems/guide/01-overview/) ·
[Bindings guide](https://zlink.systems/bindings/guide/) ·
[Repository doc index](./doc/README.md) ·
[Build Guide](./doc/building/build-guide.md)

## At a glance

The zlink repository has three layers with distinct responsibilities and levels
of abstraction.

| Layer | Responsibility | Primary targets |
|---|---|---|
| [`core/`](./core/) | Native Boost.Asio-based messaging engine and C API | Low-level messaging and native integration |
| [`bindings/`](./bindings/) | Language-native APIs and resource lifetime models for Core | C++, .NET, Java, Node.js, Python, Go, Rust |
| [`framework/`](./framework/) | Typed handlers, Channel, RouteMesh, Spot, Actor, STREAM, and the location runtime | C++, .NET, JVM (Java/Kotlin), Node.js |

```text
Application
    │
ZLink Framework
  Channel · RouteMesh · Spot · Actor · STREAM
    │
Language Binding
    │
zlink Core
  PAIR · PUB/SUB · XPUB/XSUB · DEALER/ROUTER · STREAM
    │
tcp · ipc · inproc · tls · ws · wss
```

Start with Core or a Binding when you want to compose sockets and transports
directly. Start with Framework when you want to build distributed real-time
services inside an application host and its dependency-injection model.

## Language support

### Bindings: seven languages

zlink Core exposes the C API directly. Bindings are provided for these seven
languages:

| Language | Binding guide | Source |
|---|---|---|
| C++ | [Guide](./bindings/doc/guide/cpp/index.en.md) | [`bindings/cpp`](./bindings/cpp/) |
| .NET/C# | [Guide](./bindings/doc/guide/dotnet/index.en.md) | [`bindings/dotnet`](./bindings/dotnet/) |
| Java | [Guide](./bindings/doc/guide/java/index.en.md) | [`bindings/java`](./bindings/java/) |
| Node.js/TypeScript | [Guide](./bindings/doc/guide/node/index.en.md) | [`bindings/node`](./bindings/node/) |
| Python | [Guide](./bindings/doc/guide/python/index.en.md) | [`bindings/python`](./bindings/python/) |
| Go | [Guide](./bindings/doc/guide/go/index.en.md) | [`bindings/go`](./bindings/go/) |
| Rust | [Guide](./bindings/doc/guide/rust/index.en.md) | [`bindings/rust`](./bindings/rust/) |

- C is the public Core API, not a separate Binding.
- Kotlin shares the Java Binding.
- JavaScript shares the Node.js Binding.

### Framework: four language families, four runtime implementations

Each ZLink Framework service runtime is independently implemented in its host
language. The runtimes do not share a native service runtime or service C ABI;
they share public contracts, a versioned wire protocol, and verification
fixtures.

| Framework runtime | Application integration | Documentation | Source |
|---|---|---|---|
| C++ | zlink framework host | [C++ docs](./framework/doc/framework/cpp/README.en.md) | [`framework/languages/cpp`](./framework/languages/cpp/) |
| .NET/C# | ASP.NET Core | [.NET docs](./framework/doc/framework/dotnet/README.en.md) | [`framework/languages/dotnet`](./framework/languages/dotnet/) |
| JVM | Java/Kotlin, Spring Boot | [Java docs](./framework/doc/framework/java/README.en.md) · [Kotlin docs](./framework/doc/framework/kotlin/README.en.md) | [`framework/languages/java`](./framework/languages/java/) |
| Node.js | TypeScript/JavaScript, NestJS | [Node.js docs](./framework/doc/framework/node/README.en.md) | [`framework/languages/node`](./framework/languages/node/) |

Framework therefore provides **four independent runtime implementations**. The
JVM runtime supports Java and Kotlin, while the Node.js runtime supports
TypeScript and JavaScript.

## zlink Core

Core is a native messaging engine derived from
[libzmq](https://github.com/zeromq/libzmq) v4.3.5 and rebuilt around a focused set
of messaging patterns.

- PAIR, PUB/SUB, XPUB/XSUB, DEALER/ROUTER, and STREAM sockets
- Asynchronous I/O based on Boost.Asio
- `tcp`, `ipc`, `inproc`, `tls`, `ws`, and `wss` transports
- TLS and WebSocket integration backed by OpenSSL
- Routing IDs, socket monitoring, and backpressure
- Shared messaging semantics across the C API and seven language Bindings

If you are new to Core, start with the [Core overview](./core/doc/guide/01-overview.en.md),
continue to the [socket pattern guide](./core/doc/guide/03-0-socket-patterns.en.md),
and use the [Core specification](./core/doc/spec/README.en.md) as the formal contract.

## ZLink Framework

ZLink Framework connects a real-time messaging layer to the lifecycle and
dependency injection facilities of an application host. In the same way that
Spring MVC adds a web layer to Spring, ZLink Framework integrates a real-time
messaging layer with ASP.NET Core, Spring Boot, and NestJS. For C++, the zlink
framework host also provides dependency injection, configuration, HTTP hosting,
and process lifecycle management.

Applications define typed handlers and clients. Framework manages transport
connections, peer discovery, location resolution, routing, reconnects, packet
codecs, and reply correlation.

| Capability | Purpose |
|---|---|
| **Channel / RouteMesh** | Find services by logical ChannelName and carry inter-server requests, replies, commands, and events |
| **Spot** | Process stateful units such as rooms, stages, and zones in a serialized execution context |
| **Actor** | Manage lifecycle, session binding, and relocation for state objects representing connections or users |
| **STREAM** | Manage external TCP/TLS/WS/WSS client lifecycles, framing, and packet dispatch |
| **Location runtime** | Discover current service, Spot, and Actor locations and maintain connections |
| **Graceful drain** | Restrict new work and shut down while accounting for in-flight work and state movement |

Framework fits systems that must manage connections, state, and routing together,
including real-time game servers, long-lived stateful services, and distributed
services implemented in multiple languages. Room, zone, match, and actor-based
topologies are composed from the same RouteMesh, Spot, Actor, and STREAM
primitives.

See the [Framework guide](https://zlink.systems/) (per-language server guides) for the guided
introduction and the [common Framework specification](https://zlink.systems/common/spec/server/)
for formal semantics and responsibility boundaries.

## Quick start

### Package consumers

Applications that consume a Binding or Framework package do not normally build
Core from the repository first. Packages for .NET, Java, Node.js, and Go, among
others, include a platform-native Core; each remaining language guide owns its
installation and native-runtime preparation procedure.

- To use the Core API through a language package, [choose a Binding](https://zlink.systems/bindings/guide/)
  and follow its installation procedure and five-minute example.
- To use ZLink Framework, pick a language on the [Installation](https://zlink.systems/install/)
  page; the full per-language guides are on the [website](https://zlink.systems/).
- After installation, run the samples for that language to verify the package
  and native runtime together in real client/server processes.

### Install a Framework package

The framework is installed from the packages published to each language's registry. It pulls in
the binding package and the Core native runtime, so Core is never built separately.

```bash
dotnet add package Zlink.Framework.AspNetCore                 # C# / .NET (nuget.org)
npm install @zlink-systems/framework @zlink-systems/nestjs    # Node.js / TypeScript (npm)
```

```kotlin
implementation("systems.zlink:zlink-framework-spring-boot-starter")   // Java and Kotlin (Maven Central)
```

For C++, install Core with vcpkg or Conan and the framework with the vcpkg overlay port
`zlink-framework`. The host registration code, the C++ CMake presets and each language's
"installation and first run" link are on the [Installation](https://zlink.systems/install/) page.

### Build Core from the repository

The following requirements and commands are for developers building zlink Core
from source. They are not common prerequisites for applications that only
consume a language package.

#### Requirements

- CMake 3.10 or newer
- A compiler with C++17 support
- OpenSSL when TLS/WSS is enabled

Run the following commands from the repository root:

```bash
cmake -S core -B core/build -DWITH_TLS=ON -DBUILD_TESTS=ON
cmake --build core/build
ctest --test-dir core/build --output-on-failure
```

Platform build scripts are also available:

```bash
# Linux
./core/builds/linux/build.sh x64 ON

# macOS
./core/builds/macos/build.sh arm64 ON

# Windows PowerShell
.\core\builds\windows\build.ps1 -Architecture x64 -RunTests "ON"
```

See the [build guide](./doc/building/build-guide.md) and
[CMake options](./doc/building/cmake-options.md) for dependencies and the full
option set.

### Build local Core and Binding packages

In a WSL development environment, use the local package runner to build Core and
first-party Binding packages from the current source revision.

```bash
# Core and every first-party Binding package
scripts/local-package/build-wsl.sh

# Only selected Binding packages
scripts/local-package/build-wsl.sh dotnet java node
```

See the [local package guide](./scripts/local-package/README.ko.md) for output
locations, package provenance, and per-language artifacts. This runner produces
Core and Binding packages; it does not establish Framework build completion.

The [Installation](https://zlink.systems/install/) page and the per-language
"installation and first run" guides cover package installation and the first application scenario. Framework source
builds and tests are independent runtime lanes that require a matching Binding
package and start from the corresponding source root:
[C++](./framework/languages/cpp/),
[.NET](./framework/languages/dotnet/),
[JVM](./framework/languages/java/), or
[Node.js](./framework/languages/node/).

> A successful Core build, a language package build, clean-consumer validation,
> and a real client/server sample run are separate validation stages. Before
> deployment, run the build, test, and sample procedures for the layers and
> languages you use.

## Samples

### Core and Bindings

Each Binding includes language-specific samples for PAIR, PUB/SUB,
DEALER/ROUTER, request/reply, STREAM, and monitoring.

- [`bindings/cpp/samples`](./bindings/cpp/samples/)
- [`bindings/dotnet/samples`](./bindings/dotnet/samples/)
- [`bindings/java/samples`](./bindings/java/samples/)
- [`bindings/node/samples`](./bindings/node/samples/)
- [`bindings/python/samples`](./bindings/python/samples/)
- [`bindings/go/samples`](./bindings/go/samples/)
- [`bindings/rust/samples`](./bindings/rust/samples/)

### Framework

Framework samples run clients and multiple server roles to validate complete
application scenarios, not only individual API calls. Read the common sample
contract first, then select a language implementation.

- [Common Framework samples](./framework/doc/framework/common/sample/README.en.md)
- [`framework/languages/cpp/samples`](./framework/languages/cpp/samples/)
- [`framework/languages/dotnet/samples`](./framework/languages/dotnet/samples/)
- [`framework/languages/java/samples`](./framework/languages/java/samples/)
- [`framework/languages/node/samples`](./framework/languages/node/samples/)

## Documentation map

| Goal | Document |
|---|---|
| Understand the documentation structure | [Documentation index](./doc/README.md) |
| Contribution and operations rules (build, test, gates, release procedure) | [Contributor handbook](./CONTRIBUTING.md) |
| Learn the Core API | [Core user guide](https://zlink.systems/guide/01-overview/) |
| Read the formal Core contract | [Core specification](https://zlink.systems/spec/) |
| Use a language Binding | [Bindings guide](https://zlink.systems/bindings/guide/) |
| Read the formal Binding contracts | [Bindings specification](https://zlink.systems/bindings/spec/) |
| Understand Framework and its use cases | [Framework guide](https://zlink.systems/) (chapter 1 of each language's server guide) |
| Read the formal Framework contract | [Common Framework specification](https://zlink.systems/common/spec/server/) |
| Messaging performance against gRPC | [gRPC comparison report](https://zlink.systems/bench/comparison/) (the specification and per-language bench pages sit in the same Bench section) |
| Framework internals and exact interfaces | [Per-language specifications](https://zlink.systems/common/spec/server/) (source: `framework/doc/framework/common/spec/`) |
| Build and test Core from source | [Core build guide](./doc/building/build-guide.md) |
| Build local Core and Binding packages from the current source | [Local package guide](./scripts/local-package/README.ko.md) |
| Install and use a Binding package | [Bindings guide](https://zlink.systems/bindings/guide/) |
| Install a Framework package and run the first scenario | [Installation](https://zlink.systems/install/), then the per-language [Installation and first run](https://zlink.systems/dotnet/guide/server/02-getting-started/) (switch language at the top of the chapter) |
| Enter a Framework runtime source/build lane | [C++](./framework/languages/cpp/) · [.NET](./framework/languages/dotnet/) · [JVM](./framework/languages/java/) · [Node.js](./framework/languages/node/) |
| Prepare release packages | [Packaging guide](./doc/building/packaging.md) |
| Find the build scripts and the release path | [Build and release pipeline](./doc/building/release-pipeline.md) |
| See what the framework builds by default, the two sample modes and the C++ presets | [Framework workspace layout](./doc/building/framework-workspace.md) |
| Review licensing | [License guide](./doc/license/README.md) |
| Report a security issue | [Security policy](./SECURITY.md) |

Guides and specifications are easiest to read on [zlink.systems](https://zlink.systems/); the
sources live in this repository under `core/doc`, `bindings/doc`, and `framework/doc`. Guides
explain concepts and usage. Specifications and language-specific exact interfaces own the
formal contracts; when they differ, the specification and
exact interface take precedence.

## Supported platforms

Core provides x64 and ARM64 build paths for Linux, an ARM64 build path for macOS, and an x64 build path for Windows. Consult
each language document for runtime support, package formats, and platform-specific
constraints of its Binding or Framework implementation.

## License

Licensing differs by repository layer.

| Scope | License |
|---|---|
| `core/`, `bindings/` | [Mozilla Public License 2.0](./LICENSE) |
| `framework/` | [Functional Source License 1.1, ALv2 Future License](./framework/LICENSE) |
| Language-specific Framework `http-client` packages | Apache License 2.0 |

See the [license guide](./doc/license/README.md) and
[THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md) for detailed terms, the
two-year Apache License 2.0 conversion policy, and redistribution notices.

Based on [libzmq](https://github.com/zeromq/libzmq) — Copyright (c) 2007-2024
Contributors as noted in [`core/AUTHORS`](./core/AUTHORS).
