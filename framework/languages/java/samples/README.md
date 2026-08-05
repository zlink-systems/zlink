# ZLink Java And Kotlin Samples

This directory contains Java and Kotlin samples for the public 10.0.0 framework
contract. Java samples are under `java/`, Kotlin samples are under `kotlin/`,
and both languages implement the same six sample scenarios defined by the
[common sample documents](../../../doc/framework/common/sample/README.ko.md).

## Samples

| Sample | Main framework behavior | Peer topology |
|---|---|---|
| `Bingo` | Session gateway, Entry and room Spots, Actor binding, timers, and bound-session push | Redis location store |
| `TicTacToe` | Two API roles, two Play roles, room lookup, Actor turns, and real-time messages | Manual MeshNode peers; Redis room route store |
| `SupportChat` | Conversation ownership, agent assignment, reconnect, idle timeout, and close notifications | Redis location store |
| `DeliveryDispatch` | Courier selection, timeout reassignment, tracking, and customer push | Redis location store |
| `GameQuest` | Player quest owner Spots, event streams, and projections | Redis location store |
| `ShoppingMall` | Channel service selection, order workflow, event streams, projections, and fanout events | Redis location store |

Both language directories contain these six sample roots. Their internal file
layouts follow each language and are not required to be identical:

```text
samples/
|-- java/
|   |-- Bingo/
|   |-- DeliveryDispatch/
|   |-- GameQuest/
|   |-- ShoppingMall/
|   |-- SupportChat/
|   `-- TicTacToe/
`-- kotlin/
    |-- Bingo/
    |-- DeliveryDispatch/
    |-- GameQuest/
    |-- ShoppingMall/
    |-- SupportChat/
    `-- TicTacToe/
```

The common sample documents own workflow and message contracts. An individual
sample README is present only when that language needs additional setup,
execution, or layout guidance; the absence of a per-sample README does not
change the supported sample inventory.

TicTacToe is the only sample that configures MeshNode peers manually. Every
other sample uses the Redis location store to resolve Spot and Actor locations
and establish MeshNode peers.

## MeshNode And Channel Names

Each physical mesh has one MeshNode per process. A ChannelName is logical
service membership on that MeshNode and does not create another ROUTER
endpoint. Node direct, ChannelName select-one, Spot, Actor, and Logical
Multicast operations share the MeshNode. Classic fanout uses a separate PUB/SUB
channel.

## Project Layout

Open `framework/languages/java` in IntelliJ IDEA to load the framework and all
sample modules through the included `zlink-framework-java-samples` Gradle
build. Opening this `samples/` directory directly loads only the sample build.

Individual sample directories use `standalone.settings.gradle.kts` for their
runner and do not add nested `settings.gradle.kts` roots. Shared message
contracts stay under `shared/contracts`. Server topology, ChannelName,
endpoint, packet, and timing settings stay under `server/configuration`;
client-only settings stay under `client/configuration`.

Bingo uses Protobuf payloads. The other samples use the framework's typed JSON
serialization path. Sample handlers and clients do not register a codec for
each message type.

## Running Samples

Run every Java and Kotlin sample from this directory:

```bash
./run_samples.sh
```

On Windows:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\run_samples.ps1
```

Pass language and sample paths to run a subset in the given order.

```bash
./run_samples.sh java/Bingo kotlin/SupportChat
```

Each sample runner starts role-specific Spring processes, waits for readiness,
runs the probe or client scenario, and removes the processes and Redis
container it created. Application role code starts only its own role.

Framework hosts bind endpoint, Redis, routing ID, timeout, and logging values
from role-specific Spring configuration files. Application code does not read
these values directly from environment variables or JVM system properties.

Check the IDE-importable Gradle build without running scenarios:

```bash
./gradlew projects
./gradlew buildAllSamples
```

From `framework/languages/java`, use the included build name:

```bash
./gradlew :zlink-framework-java-samples:projects
./gradlew :zlink-framework-java-samples:buildAllSamples
```
