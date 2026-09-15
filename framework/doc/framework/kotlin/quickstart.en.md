# Kotlin Quickstart — from an empty project to a first request

> **Contract owner for this chapter** — none. The formal API contract is in the
> [Kotlin spec](../common/spec/server/languages/kotlin/README.en.md).

The project lives at
[`framework/languages/java/quickstart/kotlin/`](../../../languages/java/quickstart/kotlin/). The
code blocks below are read from those files when the site is built. The Java and Kotlin
quickstarts share one Gradle build.

Without a location store, two processes name each other's endpoint directly and exchange one
request/reply. The next step is
[Installation and first run](guide/server/02-getting-started.en.md).

## Prerequisites

- JDK 25 or later. The published `zlink-framework-core` 0.12.0 has class file major 69 (Java 25)
- Access to Maven Central

## 1. Package versions

`systems.zlink:zlink` (the binding) is not listed. `zlink-framework-core` declares the
version it depends on.

```toml title="gradle/libs.versions.toml"
--8<-- "framework/languages/java/quickstart/gradle/libs.versions.toml"
```

## 2. Shared contract

```kotlin title="Shared"
--8<-- "framework/languages/java/quickstart/kotlin/Shared/src/main/kotlin/systems/zlink/quickstart/shared/Contracts.kt"
```

## 3. The handling side

`addHandlersFromPackageOf` only discovers handler types. Which channel exposes one is a
separate registration on `channelName(...).server()`.

This process serves no HTTP, so `SpringApplication.setKeepAlive(true)` is required;
without it the JVM exits right after the context refreshes.

```kotlin title="Server"
--8<-- "framework/languages/java/quickstart/kotlin/Server/src/main/kotlin/systems/zlink/quickstart/server/ServerApplication.kt"
```

## 4. The calling side

```kotlin title="Client"
--8<-- "framework/languages/java/quickstart/kotlin/Client/src/main/kotlin/systems/zlink/quickstart/client/ClientApplication.kt"
```

## 5. Run

```bash
cd framework/languages/java/quickstart
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist

# Two terminals. Start the server first.
./kotlin/Server/build/install/Server/bin/Server
./kotlin/Client/build/install/Client/bin/Client

curl http://127.0.0.1:5080/hello/world
```

The response is `hello, world` with status 200.

## What to carry over

| File | Content |
|---|---|
| `gradle/libs.versions.toml` | One `zlinkFramework` entry pins all three artifacts. The binding stays transitive |
| Shared | The message types both processes share |
| Server | `addRouteMesh` → `listen` → `channelName(...).server().addRequestHandler(...)`, plus `setKeepAlive(true)` |
| Client | `channelName(...).client()`, `peerConnections().connect(...)`, `requestToChannel(...)` |

Replacing the manual `peerConnections().connect` with a location store is covered by
[10. Location](guide/server/10-location.en.md). Further build-setup requirements are listed
in the project's `README.md`.
