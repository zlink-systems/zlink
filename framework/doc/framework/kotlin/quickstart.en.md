# Kotlin Quickstart — from Install to a First Request

!!! info "What you get from this chapter"

    You can install the packages and run a minimal project where two processes call each other.

The project lives at
[`framework/languages/java/quickstart/kotlin/`](../../../languages/java/quickstart/kotlin/). The
code blocks below are read from those files when the site is built. The Java and Kotlin
quickstarts share one Gradle build. Without a location store, two processes name each other's
endpoint directly and exchange one request/reply.

## 0. Downloading the tutorial

This chapter builds the smallest project from scratch. **To run the finished tutorial instead**,
one archive is all you need — there is no reason to clone the whole repository.

[**Download zlink-tutorial-java.zip**](https://github.com/zlink-systems/zlink/releases/latest/download/zlink-tutorial-java.zip)

The address does not depend on the platform: Windows and WSL fetch the same file. Unpacking it
leaves the project under `zlink-tutorial-java/`, with the package versions of the release you
downloaded.

For the current main, take just that directory out of the repository.

```bash
git clone --filter=blob:none --sparse https://github.com/zlink-systems/zlink.git
cd zlink
git sparse-checkout set framework/languages/java/tutorial
```

## 1. Installation

- JDK 25 or later. The published `zlink-framework-core` 0.16.0 has class file major 69 (Java 25)
- Access to Maven Central

Get it from Maven Central. The minimal combination needed to build one server is the following.

```kotlin
// The contract and runtime
implementation("systems.zlink:zlink-framework-core")
// DI/lifecycle registration
implementation("systems.zlink:zlink-framework-spring-boot-starter")
// Coroutine idiom
implementation("systems.zlink:zlink-framework-kotlin")
```

**Do not list `systems.zlink:zlink` (the binding) yourself.** `zlink-framework-core` declares the
version it depends on.

```toml title="gradle/libs.versions.toml"
--8<-- "framework/languages/java/quickstart/gradle/libs.versions.toml"
```

Artifacts to add when you need them:

| Artifact | When to add it |
| --- | --- |
| `zlink-framework-locations-redis` | When using the Redis location store for auto-connect ([Location](guide/server/25-location.en.md)) |
| `zlink-framework-codec-protobuf` · `-codec-msgpack` | To use instead of the default JSON codec ([Handlers and Message Processing](guide/server/31-handler-dispatch.en.md#3-codecs--turning-a-payload-into-bytes)) |
| `zlink-stream-connector` | When building an external client (a game client, mobile) ([STREAM](guide/server/23-stream.en.md)) |
| `zlink-http-client` | When the server calls out over HTTP ([HTTP Client guide](guide/http-client/README.en.md)) |

The license differs by layer — core/binding is MPL-2.0, framework is FSL-1.1-ALv2, and
`zlink-http-client` is Apache-2.0. There is no cost to building and selling a service
([Where ZLink Applies](guide/server/17-alternative.en.md#8-license--the-cost-of-using-it)).

## 2. Shared contract

```kotlin title="Shared"
--8<-- "framework/languages/java/quickstart/kotlin/Shared/src/main/kotlin/systems/zlink/quickstart/shared/Contracts.kt"
```

## 3. The handling side

`addHandlersFromPackageOf` only discovers handler types. Which channel exposes one is a
separate registration on `channelName(...).server()`.

This process serves no HTTP, so `SpringApplication`'s `isKeepAlive` must be set to `true`;
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

## 6. What to check when the first run fails

| Symptom | What to check |
| --- | --- |
| An artifact is not found | Check that the artifact names in section 1 were copied exactly. The binding version is not pinned separately |
| Startup fails | Check that both processes name the same mesh, and that the listen endpoint does not collide with another process |
| The server exits immediately | Check that `isKeepAlive` is set to `true` |
| A call ends with no target | Check that the receiving side registered that channel name in the server role, and that the two processes are connected as peers |
| No answer arrives | Check that the caller used `request`. A `send` receives no answer |

## 7. What to carry over

| File | Content |
|---|---|
| `gradle/libs.versions.toml` | One `zlinkFramework` entry pins all three artifacts. The binding stays transitive |
| Shared | The message types both processes share |
| Server | `addRouteMesh` → `listen` → `channelName(...).server().addRequestHandler(...)`, plus `isKeepAlive = true` |
| Client | `channelName(...).client()`, `peerConnections().connect(...)`, `requestToChannel(...)` |

Further build-setup requirements are listed in the project's `README.md`.

## 8. What to read next

These two processes connect by writing each other's endpoint directly. Keeping the calling code
unchanged while servers are added or restarted at another address needs automatic connection, and
that is covered by [Location](guide/server/25-location.en.md).

- To go over the concepts first — [Core Concepts](guide/server/03-concepts.en.md)
- The path that calls by name — [Channel Messaging](guide/server/20-channel-messaging.en.md)
- State objects called by id — [Spot](guide/server/21-spot.en.md) · [Actor](guide/server/22-actor.en.md)
- To see a complete business flow — [Picking a Sample](guide/server/14-samples.en.md)
