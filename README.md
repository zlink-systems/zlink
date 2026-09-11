**English** | [한국어](./README.ko.md)

# zlink

> A messaging engine, bindings for seven languages, and a real-time messaging
> framework — for services that manage connections, state, and routing together.

[![Build](https://github.com/zlink-systems/zlink/actions/workflows/build.yml/badge.svg)](https://github.com/zlink-systems/zlink/actions/workflows/build.yml)
[![License: MPL-2.0 / FSL-1.1 / Apache-2.0](https://img.shields.io/badge/License-multiple-blue.svg)](./doc/license/README.md)

| | |
|---|---|
| **Website** | [zlink.systems](https://zlink.systems/) |
| **Guides and API reference** | [zlink.systems](https://zlink.systems/) |
| **Installation** | [zlink.systems/install](https://zlink.systems/install/) |

The website owns the product documentation — guides, tutorials, and the API
reference. This page describes the repository: what it contains, how it is
layered, and how to build it from source.

## What is zlink

zlink is a messaging platform built in three layers. Each layer is usable on its
own, and each one adds a level of abstraction over the one below it.

| Layer | Responsibility | Languages |
|---|---|---|
| [`core/`](./core/) | Native Boost.Asio messaging engine and the public C API | C |
| [`bindings/`](./bindings/) | Language-native APIs and resource lifetime models over Core | C++, .NET, Java, Node.js, Python, Go, Rust |
| [`framework/`](./framework/) | Typed handlers, routing, stateful runtime units, and the location runtime | C++, .NET, JVM (Java/Kotlin), Node.js |

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

**Core** is a native messaging engine derived from
[libzmq](https://github.com/zeromq/libzmq) v4.3.5 and rebuilt around Boost.Asio
and a focused set of messaging patterns — PAIR, PUB/SUB, XPUB/XSUB,
DEALER/ROUTER, and STREAM sockets over `tcp`, `ipc`, `inproc`, `tls`, `ws`, and
`wss`, with routing IDs, socket monitoring, and backpressure. Start here when you
want to compose sockets and transports directly.

**Framework** builds on top of a Binding and connects the messaging layer to an
application host's lifecycle and dependency injection. In the same way that
Spring MVC adds a web layer to Spring, ZLink Framework adds a real-time
messaging layer to ASP.NET Core, Spring Boot, and NestJS; for C++, the zlink
framework host supplies the DI, configuration, HTTP hosting, and process
lifecycle itself. Start here when you want to build distributed real-time
services rather than wire sockets yourself.

## Why zlink

A real-time service has to solve connection management, state ownership, and
routing at the same time — and the three keep colliding. A room lives on one
process, a player's session lives on another, a request has to reach whichever
node currently owns the state, and all of it has to survive reconnects and
rolling restarts. Most stacks leave that to application code.

Framework provides those pieces as first-class runtime concepts. Applications
define typed handlers and clients; Framework manages transport connections, peer
discovery, location resolution, routing, reconnects, packet codecs, and reply
correlation.

| Capability | Purpose |
|---|---|
| **Channel / RouteMesh** | Find services by logical ChannelName and carry inter-server requests, replies, commands, and events |
| **Spot** | Process stateful units such as rooms, stages, and zones in a serialized execution context |
| **Actor** | Manage lifecycle, session binding, and relocation for state objects representing connections or users |
| **STREAM** | Manage external TCP/TLS/WS/WSS client lifecycles, framing, and packet dispatch |
| **Location runtime** | Discover current service, Spot, and Actor locations and maintain connections |
| **Graceful drain** | Restrict new work and shut down while accounting for in-flight work and state movement |

This fits real-time game servers, long-lived stateful services, and distributed
services implemented in several languages at once. Room, zone, match, and
actor-based topologies are composed from the same RouteMesh, Spot, Actor, and
STREAM primitives.

Framework is implemented **independently in four language runtimes**. They share
no native service runtime and no service C ABI — only public contracts, a
versioned wire protocol, and shared verification fixtures. A .NET service and a
Java service talk to each other over the same mesh.

## A quick look

A server that owns the `greeting` channel, and a client that calls it without
naming a node:

```csharp
// Server — registers a handler and announces that it serves "greeting".
builder.Services.AddZLinkFramework(options =>
{
    options.AddHandlersFromAssemblyOf<Program>();

    var mesh = options.AddRouteMesh("services").Listen("tcp://0.0.0.0:7101");
    mesh.Channel("greeting").Server();
});

public sealed class HelloHandler : IZLinkRequestHandler<Hello, Greeting>
{
    public ValueTask<Greeting> HandleAsync(
        Hello request, IZLinkMessageContext context, CancellationToken cancellationToken)
        => ValueTask.FromResult(new Greeting($"hello, {request.Name}"));
}
```

```csharp
// Client — the target is a ChannelName. Which node handles it is not specified.
var reply = await route
    .RequestToChannel("greeting", new Hello(name))
    .Async<Greeting>(cancellationToken);
```

The same server in C++, Java, Kotlin, and TypeScript:

```cpp
// C++ — zlink framework host
app.add_zlink_framework ([] (zlink_framework_options_t &options) {
    auto mesh = options.add_route_mesh ("services").listen ("tcp://0.0.0.0:7101");
    mesh.channel_name ("greeting").server ()
      .add_request_handler<hello_handler_t, hello_t, greeting_t> ();
});

class hello_handler_t
{
  public:
    using request_type = hello_t;
    using reply_type = greeting_t;

    greeting_t handle (const hello_t &request)
    {
        return greeting_t{"hello, " + request.name};
    }
};
```

```java
// Java — Spring Boot
@Bean
ZLinkFrameworkConfigurer zlink() {
    return options -> {
        options.addHandlersFromPackageOf(ServerApplication.class);

        ZLinkMeshNodeBuilder mesh = options.addRouteMesh("services").listen("tcp://0.0.0.0:7101");
        mesh.channelName("greeting").server()
            .addRequestHandler(HelloHandler.class, Hello.class, Greeting.class);
    };
}

public final class HelloHandler implements ZLinkRequestHandler<Hello, Greeting> {
    @Override
    public CompletionStage<Greeting> handle(Hello request, ZLinkMessageContext context) {
        return CompletableFuture.completedFuture(new Greeting("hello, " + request.name()));
    }
}
```

```kotlin
// Kotlin — Spring Boot
@Bean
fun zlink(): ZLinkFrameworkConfigurer = ZLinkFrameworkConfigurer { options ->
    options.addHandlersFromPackageOf(ServerApplication::class.java)

    val mesh = options.addRouteMesh("services").listen("tcp://0.0.0.0:7101")
    mesh.channelName("greeting").server()
        .addRequestHandler(HelloHandler::class.java, Hello::class.java, Greeting::class.java)
}

class HelloHandler : ZLinkRequestHandler<Hello, Greeting> {
    override suspend fun handle(request: Hello, context: ZLinkMessageContext): Greeting =
        Greeting("hello, ${request.name}")
}
```

```typescript
// Node.js — NestJS
ZLinkModule.forRootFactory({
  useFactory: () => {
    const builder = zlinkFramework();

    const mesh = builder.addRouteMesh('services').listen('tcp://0.0.0.0:7101');
    mesh.channel('greeting').server()
      .addRequestHandler(PacketNames.hello, HelloHandler);

    return builder.build();
  },
});

@zlinkRequestHandler('greeting', PacketNames.hello)
export class HelloHandler implements ZLinkRequestHandler<Hello, Greeting> {
  async handle(request: Hello): Promise<Greeting> {
    return { text: `hello, ${request.name}` };
  }
}
```

The full walkthrough, including the client half in every language, is in
[Installation and first run](https://zlink.systems/dotnet/guide/server/02-getting-started/)
(switch language at the top of the chapter).

Working one layer down, at the socket level:

```cpp
#include <zlink.hpp>

zlink::context_t ctx;
zlink::pair_socket_t server (ctx);
server.bind ("tcp://127.0.0.1:5555");

zlink::received_t inbound;
server.recv (inbound);
std::printf ("%s\n", inbound.parts ()[0].to_string ().c_str ());   // PING
inbound.close ();

zlink::message_t ack = zlink::message_t::from ("ACK");
server.send ().message (ack).submit ();
```

## Performance

Messaging throughput and latency are measured against gRPC across the Framework
languages, with the benchmark specification and the per-language results
published together: [gRPC comparison report](https://zlink.systems/bench/comparison/).

## Language support

**Bindings — seven languages.** C++, .NET/C#, Java, Node.js/TypeScript, Python,
Go, and Rust. C is the public Core API rather than a separate Binding; Kotlin
shares the Java Binding and JavaScript shares the Node.js Binding. Each Binding
carries its own samples for PAIR, PUB/SUB, DEALER/ROUTER, request/reply, STREAM,
and monitoring. → [Bindings guide](https://zlink.systems/bindings/guide/)

**Framework — four runtimes.** C++ (zlink framework host), .NET/C# (ASP.NET
Core), JVM (Java and Kotlin, Spring Boot), and Node.js (TypeScript and
JavaScript, NestJS). → [Framework guide](https://zlink.systems/)

## Getting started

Packages for each language bundle a platform-native Core, so applications do not
build this repository first.

- **Use the Core API through a language package** — [choose a Binding](https://zlink.systems/bindings/guide/) and follow its installation procedure and five-minute example.
- **Use ZLink Framework** — pick a language on the [Installation](https://zlink.systems/install/) page, then follow [Installation and first run](https://zlink.systems/dotnet/guide/server/02-getting-started/).
- **Read the formal contracts** — [Core specification](https://zlink.systems/spec/) and [common Framework specification](https://zlink.systems/common/spec/server/).

## Building from source

Building from the repository is for working on zlink itself, or for linking
against an unreleased revision.

**Requirements** — CMake 3.10+, a C++17 compiler (GCC 7+, Clang 5+, MSVC 2017+),
and OpenSSL when TLS/WSS is enabled. Framework runtimes additionally need the
toolchain of their host language.

Core provides x64 and ARM64 build paths for Linux, macOS, and Windows. Consult
each language document for the runtime support, package formats, and
platform-specific constraints of its Binding or Framework implementation.

Start with the five-minute build in the
[contributor handbook](./CONTRIBUTING.md#1-five-minute-start) — it clones,
builds Core with tests, and runs a Binding smoke test. Detailed procedures live
per layer:

| Layer | Build documentation |
|---|---|
| Core | [Build guide](./doc/building/build-guide.md) · [CMake options](./doc/building/cmake-options.md) |
| Bindings | [Local Core and Bindings](./doc/building/local-core-bindings.ko.md) (Korean) · [local package runner](./scripts/local-package/README.ko.md) (Korean) |
| Framework | [Framework workspace layout](./doc/building/framework-workspace.md), then the source root for [C++](./framework/languages/cpp/), [.NET](./framework/languages/dotnet/), [JVM](./framework/languages/java/), or [Node.js](./framework/languages/node/) |
| Packaging and release | [Packaging guide](./doc/building/packaging.md) · [Build and release pipeline](./doc/building/release-pipeline.md) |

A successful Core build, a language package build, clean-consumer validation,
and a real client/server sample run are separate validation stages. Before
deployment, run the build, test, and sample procedures for the layers and
languages you use.

## Repository layout

| Path | Contents |
|---|---|
| [`core/`](./core/) | Core engine, the public C API (`core/include`), and Core tests |
| [`bindings/`](./bindings/) | Seven language Bindings, each with samples under `bindings/<lang>/samples` |
| [`framework/`](./framework/) | Four Framework runtimes under `framework/languages/`, with samples per language |
| [`doc/`](./doc/) | Repository documentation — [index](./doc/README.md), design principles, build and release |
| [`scripts/`](./scripts/) | Local packaging, gates, and performance measurement tooling |

Framework samples run clients and multiple server roles to validate complete
application scenarios rather than individual API calls; read the
[common sample contract](./framework/doc/framework/common/sample/README.en.md)
before picking a language implementation.

## Contributing

Read the [contributor handbook](./CONTRIBUTING.md) first — it covers the build,
code and documentation rules, the pre-commit gates, and the branch, commit, PR,
and release procedure. Issues and pull requests are welcome.

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
