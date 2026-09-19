# ZLink Java And Kotlin Samples

This directory contains Java and Kotlin samples for the published
`zlink-framework-*` package version (see `gradle/zlink-sample-dependencies.settings.gradle.kts`'s
`zlink.frameworkVersion` default). Java samples are under `java/`, Kotlin
samples are under `kotlin/`, and both languages implement the same seven
sample scenarios defined by the
[common sample documents](../../../doc/framework/common/sample/README.ko.md).

## Prerequisites

- **JDK 25.** Gradle toolchain is pinned to 25
  (`gradle/zlink-jvm-baseline.settings.gradle.kts`). No Gradle toolchain
  auto-download resolver (such as
  `org.gradle.toolchains.foojay-resolver-convention`) is configured; the run
  scripts locate an existing JDK 25 themselves (`JAVA_HOME`, `PATH`,
  `~/.gradle/jdks`, and other common install locations - see
  `gradle/zlink-jvm-runtime.sh` / `Set-ZlinkSampleJavaRuntime` in
  `redis-common.ps1`) and fail with one message naming the missing version if
  none match. Install [Temurin 25](https://adoptium.net/) yourself and point
  `JAVA_HOME` at it, or a plain `installDist`/`build` (outside the run
  scripts) fails immediately with `No matching toolchain found`.
- **Docker and Redis.** Each sample run script starts and removes its own
  Redis container (`redis-common.ps1` / `runner-common.sh`); Docker must be
  running first.

No Python is required. The Linux port-reservation helper
(`runner-common.sh`'s `zlink_sample_reserve_ports_in_range`) and the Windows
ZoneWorld ZW-B8 fault proxy (`ZoneWorld/Support/SessionRouteBlockProxy.java`)
both run as JDK single-file source programs (`java <file>.java ...`), so the
JDK 25 above is the only runtime either one needs.

## Samples

| Sample | Main framework behavior | Peer topology |
|---|---|---|
| `Bingo` | Session gateway, Entry and room Spots, Actor binding, timers, and bound-session push | Redis location store |
| `TicTacToe` | Two API roles, two Play roles, room lookup, Actor turns, and real-time messages | Manual MeshNode peers; Redis room route store |
| `SupportChat` | Conversation ownership, agent assignment, reconnect, idle timeout, and close notifications | Redis location store |
| `DeliveryDispatch` | Courier selection, timeout reassignment, tracking, and customer push | Redis location store |
| `GameQuest` | Player quest owner Spots, event streams, and projections | Redis location store |
| `ShoppingMall` | Channel service selection, order workflow, event streams, projections, and fanout events | Redis location store |
| `ZoneWorld` | Gateway, two ZoneNodes, and Ops roles: Actor transfer across zones, zone Logical Multicast, Node direct operations, and runtime events | Redis location store |

Both language directories contain these seven sample roots. Their internal file
layouts follow each language and are not required to be identical:

```text
samples/
|-- java/
|   |-- Bingo/
|   |-- DeliveryDispatch/
|   |-- GameQuest/
|   |-- ShoppingMall/
|   |-- SupportChat/
|   |-- TicTacToe/
|   `-- ZoneWorld/
`-- kotlin/
    |-- Bingo/
    |-- DeliveryDispatch/
    |-- GameQuest/
    |-- ShoppingMall/
    |-- SupportChat/
    |-- TicTacToe/
    `-- ZoneWorld/
```

The common sample documents own workflow and message contracts. An individual
sample README is present only when that language needs additional setup,
execution, or layout guidance; the absence of a per-sample README does not
change the supported sample inventory.

TicTacToe is the only sample that configures MeshNode peers manually. Every
other sample uses the Redis location store to resolve Spot and Actor locations
and establish MeshNode peers.

For TicTacToe, a manual endpoint is only connection intent. When the runtime
matches that endpoint to a Redis Location Store descriptor for an object peer,
it carries the descriptor's RID, lifecycle generation, and security identity
through the admission handshake. The sample does not configure those values
or call raw transport APIs.

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

Every sample root owns a `run_sample.sh` and a `run_sample.ps1`, and one
invocation runs one sample. The
[common sample document](../../../doc/framework/common/sample/README.ko.md)
owns this rule in its "The Sample Run Script And Redis Isolation Standard"
section; what follows is only the command for this language.

From this directory, on Linux or WSL:

```bash
./java/Bingo/run_sample.sh
```

On Windows:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\Bingo\run_sample.ps1
```

Kotlin uses the same sample names under `kotlin/`:

```bash
./kotlin/Bingo/run_sample.sh
```

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\Bingo\run_sample.ps1
```

Each language has seven samples, so checking one language takes seven
invocations and checking both Java and Kotlin takes fourteen. Substitute
`Bingo`, `DeliveryDispatch`, `GameQuest`, `ShoppingMall`, `SupportChat`,
`TicTacToe`, and `ZoneWorld` in turn, one at a time.

Each sample runner starts role-specific Spring processes, waits for readiness,
runs the probe or client scenario, and removes the processes and Redis
container it created. Application role code starts only its own role. After the
probe completes, the runner allows up to 90 seconds to observe the runtime's
30-second drain deadline and its bounded owner/resource cleanup before using
SIGKILL. A Framework process that reached `ZLINK_FRAMEWORK_READY` must also
write `ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE`; a missing
marker, a non-`STOPPED/NONE` result, a force kill, or cleanup failure makes the
sample fail.

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
