# Installation

## :material-server: Framework packages { .zlink-band .band-server }

**C#/.NET · C++ · Java · Kotlin · Node/TypeScript** are supported. All five use the same
channel, Spot and Actor contracts, so nodes in one mesh call each other across languages.

| Item | Value |
|---|---|
| Supported platforms | linux-x64, linux-arm64, macos-arm64, windows-x64 (Windows ARM64 and Intel Mac unsupported) |
| Registries | nuget.org, Maven Central, npm, vcpkg/Conan plus GitHub Release |

**You install one host package.** That package includes the framework runtime and the Core
messaging engine. Each tab lists what it includes, and the optional packages you add when you
need them.

=== "C#/.NET"

    ```bash
    dotnet add package Zlink.Framework.AspNetCore
    ```

    | Included package | Role |
    |---|---|
    | `Zlink.Framework` | Framework contracts and runtime |
    | `Zlink` | **The Core messaging engine's `.NET` binding** |
    | `Zlink.Framework.Contracts` | Codec interfaces and framework exceptions |
    | `Zlink.Framework.Provider.Abstractions` | Provider extension points |
    | `Zlink.Stream.Connector` | The STREAM client surface |
    | `Zlink.HttpClient` | HTTP client |
    | `Microsoft.Extensions.DependencyInjection.Abstractions` · `.Logging.Abstractions` · `.Hosting.Abstractions` · `.Diagnostics.HealthChecks` | DI, logging, host lifecycle, health checks |

    | Optional package | When to add |
    |---|---|
    | `Zlink.Framework.Locations.Redis` | Using a Redis location store |
    | `Zlink.Framework.Codecs.Protobuf` · `Zlink.Framework.Codecs.MessagePack` | In place of the default JSON codec |

    A host that is not ASP.NET Core installs `Zlink.Framework` and starts the host in code. The
    runtime requires .NET 8 or later, and the path to a first run is covered by the
    [quickstart](dotnet/quickstart.en.md).

=== "C++"

    ```cmake
    find_package(zlink_framework CONFIG REQUIRED)
    target_link_libraries(app PRIVATE zlink::framework)
    ```

    | Included package | Role |
    |---|---|
    | `zlink::framework` | Framework contracts and runtime |
    | The `zlink` config | **The Core messaging engine** — located with a matching version |
    | The `zlink_cpp` config | Core's C++ binding — located with a matching version |
    | `zlink::http_client` | HTTP client |
    | `zlink::stream_connector` · `zlink::stream_connector_codecs` | The STREAM client surface |
    | `zlink::framework_provider_abstractions` | Provider extension points |

    One `find_package` line locates all of these, so Core and the binding need no lines of their
    own.

    | Optional target | When to add |
    |---|---|
    | `zlink::framework_locations_redis` | Using a Redis location store |
    | `zlink::framework_codec_protobuf` · `zlink::framework_codec_messagepack` | In place of the default JSON codec |
    | `zlink::stream_connector_throwing` | Receiving errors as exceptions |
    | `zlink::stream_e2e_client` | Writing e2e and performance scenario clients |

    **Third-party packages the consumer provides.** The install config does not install these;
    it only locates them with `find_dependency`, so that they do not collide with libraries
    already in the consumer's build and the consumer chooses the versions.

    | Package | Where it is used | How to turn it off |
    | --- | --- | --- |
    | `lz4` | STREAM compression | `-DZLINK_FRAMEWORK_CPP_STREAM_WITH_LZ4=OFF -DZLINK_STREAM_CONNECTOR_WITH_LZ4=OFF` |
    | `openssl` | Stream Connector TLS | `-DZLINK_STREAM_CONNECTOR_WITH_TLS=OFF` |
    | `boost` (asio, beast) | HTTP and transport | Cannot be turned off. Core and the framework must use the **same Boost tree** |
    | `nlohmann_json` | Default JSON serializer | Cannot be turned off |
    | `opentelemetry-cpp` | Observation | Cannot be turned off |
    | `protobuf` | Protobuf codec | Only when that target is linked |
    | `redis-plus-plus` · `libuv` | Redis location store | Only when that target is linked |

    Both compression options default to `ON`, and while they are on the build runs
    `find_package(lz4 REQUIRED)`, so **configure fails without it.**

    **Installing.** `zlink` is not in the official vcpkg registry or ConanCenter yet, so the
    first two paths use the overlay port and the recipe this repository provides.

    ```bash
    git clone https://github.com/zlink-systems/zlink.git

    # vcpkg
    vcpkg install zlink zlink-cpp zlink-framework \
      --overlay-ports=zlink/vcpkg/ports --triplet=x64-linux

    # Conan
    conan create zlink/core/packaging/conan --build=missing -s compiler.cppstd=gnu20
    conan create zlink/bindings/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
    conan create zlink/framework/languages/cpp/packaging/conan --build=missing -s compiler.cppstd=gnu20
    ```

    The third path builds and installs the GitHub Release source archives in the order
    `core/vX.Y.Z` → `cpp/vX.Y.Z` → `framework-cpp/vA.B.C`.

    In an IDE, pick a preset from `CMakePresets.json`: `vs2022` for Visual Studio,
    `windows-ninja`, `linux-ninja` or `macos-ninja` for Rider, VS Code and CLion. A C++20
    compiler is required, and the full procedure for all three paths is covered by the
    [quickstart](cpp/quickstart.en.md).

=== "Java"

    ```kotlin
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.16.0")
    ```

    | Included package | Role |
    |---|---|
    | `zlink-framework-core` | Framework contracts and runtime |
    | `systems.zlink:zlink` | **The Core messaging engine's Java binding** |
    | `zlink-framework-provider-abstractions` | Provider extension points |
    | `zlink-http-client` | HTTP client |
    | `io.netty:netty-buffer` · `com.fasterxml.jackson.core:jackson-databind` | Buffers and JSON |
    | `org.lz4:lz4-java` · `org.slf4j:slf4j-api` | Compression and logging |
    | `spring-boot-autoconfigure` · `spring-context` · `spring-boot-actuator` | DI, lifecycle, health checks |

    | Optional artifact | When to add |
    |---|---|
    | `zlink-framework-locations-redis` | Using a Redis location store |
    | `zlink-framework-codec-protobuf` · `zlink-framework-codec-msgpack` | In place of the default JSON codec |
    | `zlink-stream-connector` | When the server is also a STREAM client |
    | `zlink-framework-testkit` | Starting the runtime inside tests |

    All of them are in the `systems.zlink` group. The runtime requires JDK 25 or later, and the
    path to a first run is covered by the [quickstart](java/quickstart.en.md).

=== "Kotlin"

    ```kotlin
    implementation("systems.zlink:zlink-framework-spring-boot-starter:0.16.0")
    implementation("systems.zlink:zlink-framework-kotlin:0.16.0")
    ```

    | Included package | Role |
    |---|---|
    | `zlink-framework-core` | Framework contracts and runtime |
    | `systems.zlink:zlink` | **The Core messaging engine's Java binding** |
    | `zlink-framework-provider-abstractions` | Provider extension points |
    | `zlink-http-client` | HTTP client |
    | `zlink-stream-connector` | The STREAM client surface |
    | `io.netty:netty-buffer` · `com.fasterxml.jackson.core:jackson-databind` | Buffers and JSON |
    | `org.lz4:lz4-java` · `org.slf4j:slf4j-api` | Compression and logging |
    | `spring-boot-autoconfigure` · `spring-context` · `spring-boot-actuator` | DI, lifecycle, health checks |
    | `kotlinx-coroutines-core` · `kotlinx-coroutines-jdk8` | The suspend-function boundary |
    | `jackson-module-kotlin` | Handles Kotlin data classes as JSON |

    | Optional artifact | When to add |
    |---|---|
    | `zlink-framework-locations-redis` | Using a Redis location store |
    | `zlink-framework-codec-protobuf` · `zlink-framework-codec-msgpack` | In place of the default JSON codec |
    | `zlink-framework-testkit` | Starting the runtime inside tests |
    | `zlink-http-client-kotlin` | Calling the HTTP client from coroutines |

    Apart from `zlink-http-client-kotlin`, the optional artifacts are the same as Java's. The
    runtime requires the same JDK 25 or later, and the path to a first run is covered by the
    [quickstart](kotlin/quickstart.en.md).

=== "Node/TypeScript"

    ```bash
    npm install @zlink-systems/nestjs
    ```

    | Included package | Role |
    |---|---|
    | `@zlink-systems/framework` | Framework contracts and runtime |
    | `@zlink-systems/zlink` | **The Core messaging engine's Node binding** |
    | `@zlink-systems/stream-wire` | The STREAM wire layer |
    | `@zlink-systems/http-client` | HTTP client |
    | `@opentelemetry/api` · `@opentelemetry/api-logs` | Observation |
    | `@nestjs/common` · `reflect-metadata` · `rxjs` | DI and module registration |

    | Optional package | When to add |
    |---|---|
    | `@zlink-systems/framework-locations-redis` | Using a Redis location store |
    | `@zlink-systems/framework-codec-protobuf` · `@zlink-systems/framework-codec-msgpack` | In place of the default JSON codec |
    | `@zlink-systems/stream-connector` | When the server is also a STREAM client |

    A server that does not use NestJS, Express for instance, installs
    `@zlink-systems/framework` and starts the host in code. The runtime requires Node.js 22 or
    later, and the path to a first run is covered by the [quickstart](node/quickstart.en.md).

## :material-gamepad-variant: Client stream connector { .zlink-band .band-client }

Which connector to install is decided by the **engine and build target**, not by the language.
As a single rule — **anything built for the web (browser or WASM) uses the TypeScript
connector, whatever the language**, because no language opens an OS socket inside the browser
sandbox.

=== "Unity"

    | Build | Connector | Install |
    |---|---|---|
    | Native | `.NET` | `dotnet add package Zlink.Stream.Connector` |
    | WebGL | TypeScript | `com.zlink.stream-connector.webgl` UPM package |

    A native build uses the `.NET` connector package as it is. There is no Unity-specific
    package.

    A WebGL build installs through **Add package from git URL** in the Package Manager. Without a
    pinned tag the browser bundle drifts from the server version.

    ```
    https://github.com/zlink-systems/zlink.git?path=framework/languages/unity/com.zlink.stream-connector.webgl#framework-node/v0.16.0
    ```

    The UPM adapter carries the npm package's browser bundle and provides only the jslib and C#
    call boundary. **Its C# surface is the same as the native package's.** Both builds are
    covered by [Unity native](dotnet/guide/stream-connector/02-unity.en.md) and
    [Unity WebGL](node/guide/stream-connector/03-unity-webgl.en.md).

=== "Unreal"

    The C++ connector's Unreal plugin is compiled into the project as source. There is no web
    build target.

    | Artifact | Distribution |
    |---|---|
    | `zlink-unreal-stream-connector` | Source plugin |

    The plugin is an adapter built on `zlink::stream_connector` and
    `zlink::stream_connector_codecs`, and it comes out of the C++ connector build. The
    procedure is covered by the
    [engine adapter guide](cpp/guide/stream-connector/09-engine-adapters.en.md).

=== "Godot"

    | Build | Connector | Artifact |
    |---|---|---|
    | GDExtension (C++) | C++ | `zlink-godot-stream-connector` source GDExtension |
    | Godot C# | `.NET` | `Zlink.Stream.Connector` (NuGet) |
    | Godot Web | TypeScript | `@zlink-systems/stream-connector` (npm) |

    The GDExtension comes out of the C++ connector build. Godot C# uses the `.NET` connector
    package as it is. The procedures are covered by
    [Godot C#](dotnet/guide/stream-connector/03-godot-csharp.en.md) and the
    [engine adapter guide](cpp/guide/stream-connector/09-engine-adapters.en.md).

=== "Cocos"

    | Build | Connector | Artifact |
    |---|---|---|
    | Axmol (native) | C++ | `zlink-axmol-connector` source package |
    | Cocos Creator web | TypeScript | `@zlink-systems/stream-connector` (npm) |

    The Axmol adapter comes out of the C++ connector build. The procedure is covered by the
    [engine adapter guide](cpp/guide/stream-connector/09-engine-adapters.en.md).

=== "Web"

    The targets are browser web clients and WASM builds. Cocos Creator web, Godot Web and Unity
    WebGL share the same package root.

    ```bash
    npm install @zlink-systems/stream-connector
    ```

    | Package | Role |
    |---|---|
    | `@zlink-systems/stream-connector` | The connector itself. **Required** |
    | `@zlink-systems/stream-wire` | The wire layer, **included** by the connector |
    | `@zlink-systems/framework-codec-msgpack` | In place of the default JSON codec; it ships a separate browser entry point |

    **Browser targets accept `ws` and `wss` only.** A `tcp://` or `tls://` endpoint fails
    immediately as a configuration error. The procedure is covered by the
    [browser guide](node/guide/stream-connector/02-browser.en.md).

=== "e2e and tooling"

    Clients that connect without an engine: scenario tests, performance measurement, and
    operational tools.

    | Language | Package | Registry |
    |---|---|---|
    | `.NET` | `Zlink.Stream.Connector` | nuget.org |
    | C++ | `zlink::stream_e2e_client` — the connector plus scenario helpers | vcpkg · Conan · source |
    | Java · Kotlin | `systems.zlink:zlink-stream-connector:0.16.0` | Maven Central |
    | Node.js | `@zlink-systems/stream-connector` | npm |

    Only C++ has a dedicated target. `zlink::stream_e2e_client` adds scenario-writing helpers on
    top of `zlink::stream_connector`; the other languages do the same work with the connector
    package alone.

    The Java module is independent of the server framework, so clients without Spring Boot use
    it too. A Kotlin client that wants the coroutine surface adds `zlink-framework-kotlin`,
    which also includes `zlink-framework-core`.
