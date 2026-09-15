# Installation

This page is the first step of putting ZLink into an application. The framework is installed
from the packages published to each language's package registry. The framework package pulls in
the binding it depends on, and the binding package carries the Core engine's native runtime, so
Core is never built separately.

| Item | Value |
|---|---|
| Supported platforms | linux-x64, linux-arm64, macos-arm64, windows-x64 (Windows ARM64 and Intel Mac unsupported) |
| Registries | nuget.org, Maven Central, npm, vcpkg/Conan plus GitHub Release |

## Framework packages

The tabs below give the install commands and the host registration code per language. Writing
and running the first handler is covered by each language's "Installation and first run" chapter.

=== "C#/.NET"

    ```bash
    dotnet add package Zlink                       # Core messaging engine (.NET binding)
    dotnet add package Zlink.Framework             # contracts and runtime
    dotnet add package Zlink.Framework.AspNetCore  # DI and hosted-service registration
    ```

    ```csharp
    builder.Services.AddZLinkFramework(options =>
    {
        // Discovers handler types.
        options.AddHandlersFromAssemblyOf<Program>();
        // Which channel exposes a discovered handler is a separate registration.
        options.AddRouteMesh("services").Listen("tcp://0.0.0.0:7101")
            .Channel("greeting").Server()
            .AddRequestHandler<GreetingHandler, Hello, Greeting>();
    });
    ```

    `AddZLinkFramework` registers the framework host with ASP.NET Core's DI and lifecycle.
    `AddHandlersFromAssemblyOf` only discovers handler types. Which channel exposes one is a
    separate registration on `Channel(...).Server()`.
    The runtime needs .NET 8 or later. Continue with
    [Installation and first run](dotnet/guide/server/02-getting-started.en.md).

=== "C++"

    ```cmake
    find_package(zlink CONFIG REQUIRED)            # Core: vcpkg `zlink` or Conan `zlink`
    find_package(zlink_framework CONFIG REQUIRED)  # Framework: vcpkg overlay port `zlink-framework`
    target_link_libraries(app PRIVATE zlink::framework)
    ```

    Core comes from the vcpkg/Conan recipes; the framework from the GitHub Release source archive
    or the repository's vcpkg overlay port. IDEs open the project through the `CMakePresets.json`
    presets (`vs2022`, `windows-ninja`, `linux-ninja`, `macos-ninja`). Continue with
    [Installation and first run](cpp/guide/server/02-getting-started.en.md).

    **Third-party packages.** The installed configs never install a third-party library alongside
    the framework; they only locate one through `find_dependency`. That keeps a consumer that
    already links the same library from getting a second copy, and leaves the version choice with
    the consumer.

    The vcpkg overlay port `zlink-framework` declares all of these, so installing through it needs
    nothing extra. **Installing the source archive with plain CMake means providing them yourself.**

    | Package | Used for | Turn off with |
    | --- | --- | --- |
    | `lz4` | STREAM compression (`use_lz4()`) | `-DZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4=OFF -DZLINK_STREAM_CONNECTOR_WITH_LZ4=OFF` |
    | `openssl` | Stream Connector TLS | `-DZLINK_STREAM_CONNECTOR_WITH_TLS=OFF` |
    | `boost` (asio, beast) | HTTP and transport | `-DZLINK_FRAMEWORK_CPP_USE_SYSTEM_BOOST=OFF` to use the packaged copy |
    | `nlohmann_json` | default JSON serializer | not optional |
    | `opentelemetry-cpp` | observability | not optional |
    | `protobuf` | protobuf codec | |
    | `redis-plus-plus` | Redis location store | |

    Both compression options default to `ON`, and while they are on the build calls
    `find_package(lz4 REQUIRED)`, so **configure fails without it**. Turn them off as above to build
    without `lz4`. The installed `Findlz4.cmake` prefers a CMake config package and falls back to a
    system installation.

=== "Java"

    ```kotlin
    dependencies {
        implementation("systems.zlink:zlink-framework-core")                // contracts and runtime
        implementation("systems.zlink:zlink-framework-spring-boot-starter") // DI and lifecycle registration
    }
    ```

    Spring Boot auto-configuration registers the host as a bean. The runtime needs JDK 25 or
    later. Continue with
    [Installation and first run](java/guide/server/02-getting-started.en.md).

=== "Kotlin"

    ```kotlin
    dependencies {
        implementation("systems.zlink:zlink-framework-core")
        implementation("systems.zlink:zlink-framework-spring-boot-starter")
        implementation("systems.zlink:zlink-framework-kotlin")              // coroutine idiom
    }
    ```

    The runtime needs the same JDK 25 or later as Java. Continue with
    [Installation and first run](kotlin/guide/server/02-getting-started.en.md).

=== "Node/TypeScript"

    ```bash
    npm install @zlink-systems/framework   # contracts and runtime (brings the @zlink-systems/zlink binding)
    npm install @zlink-systems/nestjs      # DI and module registration
    ```

    Without NestJS, install only the framework package and start the host yourself. The runtime
    needs Node.js 22 or later. Continue with
    [Installation and first run](node/guide/server/02-getting-started.en.md).

## Bindings only

To use the Core API through a language package without the framework, pick a language in the
[Bindings guide](../../../bindings/doc/guide/README.en.md). Seven languages (C++, .NET, Java, Node.js,
Python, Go, Rust) have an installation procedure and a five-minute example there.

## Building from the repository

Building Core from source, or building local packages from the current source, is owned by the
repository's [build guide](../../../doc/building/build-guide.md)
and [local package guide](../../../scripts/local-package/README.md).
Package consumers do not need either.
