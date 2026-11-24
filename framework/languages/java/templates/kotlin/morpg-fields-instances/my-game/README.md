# my-game

A Kotlin MORPG template using public ZLink Framework 0.34.0 packages. Play nodes (field) host four fixed Field User Spots; each server-issued Party ID identifies a dynamic Party User Spot, and each Run ID identifies a dynamic Dungeon User Spot. `FieldPositionDomain` owns movement authority, `PartySpot` owns party membership and run phases, and `DungeonRun` owns the active run and stage.

```mermaid
flowchart LR
  subgraph playNodes["Play nodes"]
    fieldA["Play node A (field)"]
    fieldB["Play node B (field)"]
    instanceA["Play node A (dungeon)"]
    instanceB["Play node B (dungeon)"]
  end
  players -->|STREAM|session["Session node"]
  session -->|relay|fieldA
  fieldA -->|relocation|fieldB
  fieldB -->|border pub/sub|fieldA
  fieldB -->|GetOrCreate|instanceA
  instanceA <-->|host relocation|instanceB
  fieldB -->|Location / lease|location[Redis]
```

[한국어](README.ko.md)

This template is a reference. To build a new game, create a new project and design its nodes, Spots, Actors and messages for that game, using this template's structure and rules as reference; do not rename or edit this template in place into the game.

## Prerequisites

Docker with Compose v2 and Linux containers, network access to Maven Central and the Gradle Plugin Portal, and PowerShell 7 on Windows. JDK 25 is required only for local process debugging. The Gradle wrapper is included; public package versions are pinned in `build.gradle.kts`.

## Build

```sh
./run-e2e.sh --build-only
./run-e2e.sh --check-domain
```

```powershell
.\run-e2e.cmd -BuildOnly
.\run-e2e.cmd -CheckDomain
```

## Local server and e2e

Run the first command from this directory to build and start Redis and the application nodes. Run the second command from a separate terminal to connect the scenarios to that deployment. When the server is ready, `Server ready` shows the WebSocket address for game clients. Press Ctrl+C in the server terminal to stop its processes and Redis. On Windows the `.cmd` files use PowerShell 7 (`pwsh`).

```bash
./run-server.sh
./run-e2e.sh
```

```powershell
.\run-server.cmd
.\run-e2e.cmd
```

With no running server, `run-e2e` creates and removes an isolated Docker deployment. The `update` scenario replaces a play node and requires an isolated run. Use `--isolated` (`-Isolated` on Windows) to force isolation while a server is running.

The local server permits multiple Dungeons per instance. Isolated scenarios retain the configured Dungeon limit for placement and replacement checks.

The server publishes its endpoints in `.logs/server.json`. Each run keeps node stdout, stderr, Framework message-flow records and failure details under `.logs/run-<id>/`. For an attached run, find the node logs under the directory printed by `run-server` and the scenario details under the directory printed by `run-e2e`. Docker build and cleanup logs are under `.logs/docker-<project>/`.

## Client connection

### Endpoint and first packet

The Session client STREAM listener defaults to `ws://127.0.0.1:0` in `server/config/session.json`. Port `0` asks the OS for a free port; use the actual `streamEndpoint` in the process line `READY <json>`, which also contains `id` and `meshEndpoint`. Use the WebSocket address printed by `run-server` for browser clients.

Loopback is reachable only inside the same host/network namespace. Keep a deployment running for external clients, bind to a reachable interface and port, and expose that port from containers. The Docker e2e deployment is temporary and is removed after the run. Mesh listeners and Redis are not game-client endpoints.

Send `JoinWorldRequest(playerId, x)` first, for example `x = 0.0`. The server selects the field and pushes `WorldState`. Party formation and dungeon entry happen later; `ActionRejected` reports rejection.

After admission, `SessionInfoRequest` uses `connector.kotlin().request<SessionInfo>(SessionInfoRequest()).await()` for a correlated `SessionInfo` reply. The first game operation above uses `send(...).await()` and server pushes. This template does not authenticate player identity; add identity validation before Actor binding for production.

### Connector installation

Use the same connector release as this template pins in `build.gradle.kts`, `extra["frameworkVersion"]`; `e2e/build.gradle.kts` uses that key for `systems.zlink:zlink-stream-connector`. Replace `<version>` below with that pinned version. Engine installation instructions are in the linked repository READMEs; all clients use this template's packets.

| Client                             | Connector installation                                                                                                                                                                                                                                                                                                                                                                                                                                                          | Documentation guide                                                                                                                                                          |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Unity                              | Native/Editor: restore `Zlink.Stream.Connector` with NuGetForUnity. WebGL: add the `com.zlink.stream-connector.webgl` UPM Git package; include only the connector assembly for the active target. See the [Unity README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unity/README.md) and its package manifest.                                                                                                                                | [.NET engine integration](https://docs.zlink.systems/dotnet/guide/stream-connector/12-engine-integration/)                                                                   |
| Unreal                             | Copy `framework/languages/cpp/connector/engines/unreal` into `Plugins/ZLinkStreamConnector`, then package the native connector under `ThirdParty/ZLink` as described in the [Unreal README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unreal/README.md).                                                                                                                                                                                     | [C++ engine integration](https://docs.zlink.systems/cpp/guide/stream-connector/12-engine-integration/)                                                                       |
| Godot                              | .NET: add the NuGet package `Zlink.Stream.Connector` and run `dotnet restore`; see the [C# README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Godot/csharp/README.md). GDExtension: build the C++ Godot adapter against the framework prebuilt and matching `godot-cpp`; see the [C++ README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Godot/cpp/README.md). The .NET sample does not support Web export. | [.NET](https://docs.zlink.systems/dotnet/guide/stream-connector/12-engine-integration/), [C++](https://docs.zlink.systems/cpp/guide/stream-connector/12-engine-integration/) |
| Cocos Creator                      | Web target: `npm install @zlink-systems/stream-connector@<version>`; see the [Cocos Creator README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/CocosCreator/README.md). This package does not provide a Creator native adapter.                                                                                                                                                                                                                  | [TypeScript engine integration](https://docs.zlink.systems/node/guide/stream-connector/12-engine-integration/)                                                               |
| Axmol                              | Copy `framework/languages/cpp/connector/engines/axmol` into `third_party/zlink_axmol_connector` and link `zlink::stream_connector` from the installed C++ framework package; see the [Axmol README](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Axmol/README.md).                                                                                                                                                                              | [C++ engine integration](https://docs.zlink.systems/cpp/guide/stream-connector/12-engine-integration/)                                                                       |
| Web browser (TypeScript connector) | `npm install @zlink-systems/stream-connector@<version>`; import the package root in the browser build. Use `ws://` or `wss://`, subject to the page's security policy.                                                                                                                                                                                                                                                                                                             | [TypeScript connector](https://docs.zlink.systems/node/guide/stream-connector/)                                                                                              |
| C++                                | Install the C++ framework prebuilt containing the connector; use `find_package(zlink_stream_connector_cpp CONFIG REQUIRED)` and link `zlink::stream_connector`.                                                                                                                                                                                                                                                                                                                 | [C++ connector](https://docs.zlink.systems/cpp/guide/stream-connector/)                                                                                                      |
| C#/.NET                            | `dotnet add package Zlink.Stream.Connector --version <version>`                                                                                                                                                                                                                                                                                                                                                                                                                    | [.NET connector](https://docs.zlink.systems/dotnet/guide/stream-connector/)                                                                                                  |
| Java                               | Add Maven/Gradle dependency `systems.zlink:zlink-stream-connector:<version>`.                                                                                                                                                                                                                                                                                                                                                                                                      | [Java connector](https://docs.zlink.systems/java/guide/stream-connector/)                                                                                                    |
| Kotlin                             | Add `systems.zlink:zlink-stream-connector:<version>` and `systems.zlink:zlink-framework-kotlin:<version>` for the coroutine wrapper.                                                                                                                                                                                                                                                                                                                                                  | [Kotlin connector](https://docs.zlink.systems/kotlin/guide/stream-connector/)                                                                                                |
| Node                               | `npm install @zlink-systems/stream-connector@<version>`; use the package root and Node.js 22 or later.                                                                                                                                                                                                                                                                                                                                                     | [Node connector](https://docs.zlink.systems/node/guide/stream-connector/)                                                                                                    |

### Kotlin connection example

The example uses the same public connector and Kotlin coroutine wrapper as `e2e/src/main/kotlin/MyGame/e2e/Client.kt`. Save it as `e2e/src/main/kotlin/MyGame/e2e/ConnectOnce.kt` in a copy of the template. It defines a suspending function to call from your application coroutine while the deployment is running; compile it with `./gradlew :e2e:compileKotlin`.

```kotlin
package MyGame.e2e

import MyGame.*
import java.net.URI
import java.time.Duration
import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.withTimeout
import systems.zlink.framework.kotlin.*
import systems.zlink.stream.connector.*

suspend fun connectOnce(endpoint: String, playerId: String) {
    val d = ZLinkStreamConnectorOptions.createDefault(URI(endpoint))
    val connector = ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI(endpoint), ZLinkStreamDispatchMode.IMMEDIATE, Duration.ofSeconds(5),
            d.maxReconnectAttempts(), d.connectTimeout(), d.maxSendPayloadSize(),
            d.heartbeatEnabled(), d.heartbeatInterval(), d.heartbeatTimeout(), false,
            d.reconnectInitialDelay(), d.reconnectMaxDelay(), d.reconnectBackoffFactor(),
        )
    )
    val response = CompletableDeferred<WorldState>()
    val registrations = listOf(
        connector.on(WorldState::class.java) { message ->
            response.complete(message.payload())
            CompletableFuture.completedFuture(null)
        },
        connector.on(ActionRejected::class.java) { message ->
            response.completeExceptionally(IllegalStateException(message.payload().toString()))
            CompletableFuture.completedFuture(null)
        },
        connector.onErrorReceived { event ->
            response.completeExceptionally(IllegalStateException(event.toString()))
            CompletableFuture.completedFuture(null)
        },
        connector.onDisconnected { event ->
            response.completeExceptionally(IllegalStateException(event.toString()))
            CompletableFuture.completedFuture(null)
        },
    )
    try {
        connector.kotlin().connect().await()
        connector.kotlin().send(JoinWorldRequest(playerId, 0.0)).await()
        println(withTimeout(5_000) { response.await() })
    } finally {
        try { connector.kotlin().close().await() }
        finally { registrations.forEach { it.close() } }
    }
}
```

## Scenario selection and validation

Docker builds a shared node image for all roles and a runner image. The pinned Eclipse Temurin JDK/JRE bases are defined by the Dockerfile ARGs. The runner uses the Docker socket to create sibling nodes sharing its network namespace, preserving loopback listeners and stdin/stdout control. Each invocation removes its containers, network and image tags; cleanup errors return exit 1. Logs remain in `.logs/` by default; set `MY_GAME_E2E_LOGS` to choose another directory.

For local process debugging, run `./run-e2e.sh --local` or `.\run-e2e.cmd -Local`. Local mode uses JDK 25 and Compose Redis. Scenario and structure options work in both modes.

```sh
./run-e2e.sh
./run-e2e.sh --scenario field,dungeon,reconnect,update
```

```powershell
.\run-e2e.cmd
.\run-e2e.cmd -Scenario field,dungeon,reconnect,update
```

The Kotlin runner checks the structure, starts an isolated Redis Compose project and starts the Field, Instance and Session entrypoints. Redis and Framework listeners bind ephemeral loopback ports. Every run uses a unique Redis prefix and `.logs/run-<id>/` directory. Normal message-flow diagnostics are recorded in node stderr. Cleanup failures produce a nonzero exit code.

| Scenario | Assertion |
| --- | --- |
| field | The server issues a Party ID, players cross from field-2 to field-3 with the same Session, p2 observes the border publication, and party membership persists across the Field change. Movement limits and retry budget are checked. |
| dungeon | Two players join a server-issued party and Run ID, enter the same Dungeon, advance through completion, and verify that the completed run is reported to the Party Spot. |
| reconnect | A new physical Session binds the same Actor and observes the same Dungeon, members and stage. |
| update | The occupied instance drains through all relocation signals; both Sessions and Dungeon state persist; the replacement instance accepts a new party. |

The domain check covers movement rejection order, sequence bounds, relative-time relocation snapshots, PartyRun transitions, idempotent Dungeon preparation, run completion, enum names, and nullable JSON. Run it separately with `./run-e2e.sh --check-domain` or `.\run-e2e.cmd -CheckDomain`. Non-field scenarios share their party fixture. Selecting reconnect or update alone creates that fixture first. Structure-only validation is available through `./run-e2e.sh --check-structure` or `.\run-e2e.cmd -CheckStructure`.

## Framework behavior to know

- **Waiting for DB or file I/O in a handler** — An ordinary async terminal retains the turn, while `Yield` lets other handlers on the same Spot proceed. [Guide](https://docs.zlink.systems/kotlin/guide/server/32-execution-model/#io-worker-wait).
- **Creation that misses its deadline** — `DeadlineExceeded` and caller cancellation have distinct outcomes. [Guide](https://docs.zlink.systems/kotlin/guide/server/22-actor/#creation-deadline). [server/session/src/main/kotlin/MyGame/session/Program.kt:73](server/session/src/main/kotlin/MyGame/session/Program.kt).
- **Many logins at once** — Each creation for a different ID requires a Location Store round trip, so measure expected load when setting the deadline. [Guide](https://docs.zlink.systems/kotlin/guide/server/25-location/#creation-load).
- **Sending under load** — Send waits for capacity, and the application sees the final result. [Guide](https://docs.zlink.systems/kotlin/guide/server/33-backpressure/#send-backpressure). [server/session/src/main/kotlin/MyGame/session/Program.kt:49](server/session/src/main/kotlin/MyGame/session/Program.kt).
- **Duplicate login** — Without a callback, a bind from another Session is accepted; reject it in the Actor duplicate-bind callback to prevent this. [Guide](https://docs.zlink.systems/kotlin/guide/server/39-session-binding/#duplicate-session-bind). This template keeps the default (accepts the new Session).
- **ObjectGeneration** — `ObjectGeneration` distinguishes logical incarnations of the same ID; it is not a data version or write authority. [Guide](https://docs.zlink.systems/kotlin/guide/server/34-activation-lifetime/#object-generation). [server/session/src/main/kotlin/MyGame/session/Program.kt:62](server/session/src/main/kotlin/MyGame/session/Program.kt).
- **READY and Serving** — The Framework host prints `READY` after observing runtime `Serving`. [Guide](https://docs.zlink.systems/kotlin/guide/server/12-operations/#wait-for-serving). [server/shared/src/main/kotlin/MyGame/Hosting.kt:131](server/shared/src/main/kotlin/MyGame/Hosting.kt); [server/shared/src/main/kotlin/MyGame/Hosting.kt:155](server/shared/src/main/kotlin/MyGame/Hosting.kt).

## Known issues

Validated against the published Framework 0.34.0 packages.
Border subscriptions use Framework 0.34.0 `addSubscribe(handlerType, channelName, topic)` at Field activation. One handler serves all adjacent-field topics.

## Deployment sequence

Configure Redis and a unique key prefix in `server/config`. Start fieldA and bootstrap field-1..2, then fieldB and bootstrap field-3..4. Each play node (field) has capacity 2 and an Entry Spot. Start instanceA and instanceB, each with Dungeon capacity 1, then session. Every role has a separate `MyGame.<role>.ProgramKt` entrypoint in its installed distribution and accepts a configuration path. READY reports the actual Framework endpoints.

Send `drain` to the play node (dungeon)ing the party. Wait for `RELOCATION_REQUESTED`, `RELOCATED`, and `SAFE_TO_SHUTDOWN`. Verify Session identity and Dungeon state, send `shutdown`, and verify `STOPPED` and successful process exit. Start the replacement with the same configuration. The other instance retains the relocated Dungeon; its capacity is occupied, so the new party is allocated to the replacement instance.

Player Actor factories and the shared Entry implementation are registered on both play nodes (field and dungeon). Declare one Entry in `zlink.structure.json` for each play node (field or dungeon), where the application registers Entry and its first-entry handler. Do not declare Framework default Entries on other roles. The update scenario uses Dungeon capacity 1: after relocation, the occupied peer cannot create the new Dungeon, so the replacement must accept it.

Control owns the explicit 30-second Message Follow duration. Safe-shutdown observation adds the host relocation budget to that duration.

Initial Field placement uses public stable-type capacity and sequential startup. Set each play node (field) capacity to exactly its configured Field ID count. Start one play node (field) and wait for its READY, which follows creation of all its Fields, before starting the next play node (field). Earlier nodes are full, so the newly started node is the only eligible play node (field). Do not start initial play nodes (field) concurrently.
