This directory contains samples for the published
`zlink-framework-*` package version (see `gradle/zlink-sample-dependencies.settings.gradle.kts`'s
`zlink.frameworkVersion` default). The seven sample scenarios are defined by the
[common sample documents](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/README.ko.md).

<!-- zlink-lang: java -->
# ZLink Java Samples

Java samples are under `java/`.
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
# ZLink Kotlin Samples

Kotlin samples are under `kotlin/`.
<!-- zlink-lang: end -->

한국어: [`README.ko.md`](./README.ko.md)

## Prerequisites

Bash blocks run on Linux, macOS, and WSL; PowerShell blocks run on Windows PowerShell 7. `cmd` is not supported.

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
- **Docker Desktop.** Each sample run script starts and removes its own
  Redis container (`redis-common.ps1` / `runner-common.sh`); Docker must be
  running first.

No Python is required. The Linux port-reservation helper
(`runner-common.sh`'s `zlink_sample_reserve_ports_in_range`) and the Windows
ZoneWorld ZW-B8 fault proxy (`ZoneWorld/Support/SessionRouteBlockProxy.java`)
both run as JDK single-file source programs (`java <file>.java ...`), so the
JDK 25 above is the only runtime either one needs.

## Download and install

<!-- zlink-lang: java -->
Clone `zlink-java-examples` and run from its `samples/` directory.
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
Clone `zlink-kotlin-examples` and run from its `samples/` directory.
<!-- zlink-lang: end -->
It builds against the published `zlink-framework-*` packages from
Maven Central (version: see
`zlink.frameworkVersion`'s default in
`gradle/zlink-sample-dependencies.settings.gradle.kts`). There is nothing
separate to download or install: the Gradle wrapper fetches Gradle, and
Gradle fetches the packages above.

All commands below run from inside that `samples/` directory.

## Build

The run scripts (see "Run" below) build as part of running, so a separate
build step isn't required. To check the IDE-importable Gradle build without
running any scenario:

**Linux · macOS · WSL — bash**

```bash title="linux"
./gradlew projects
./gradlew buildAllSamples
```

**Windows — PowerShell 7**

```powershell title="windows"
.\gradlew.bat projects
.\gradlew.bat buildAllSamples
```

## Run

Every sample root owns a `run_sample.sh` and a `run_sample.ps1`, and one
invocation runs one sample end to end. The
[common sample document](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/README.ko.md)
owns this rule in its "The Sample Run Script And Redis Isolation Standard"
section. Redis needs no separate setup: each script starts and removes its
own container (see "Prerequisites" — Docker is the only requirement).

Linux/WSL:

**Linux · macOS · WSL — bash**

<!-- zlink-lang: java -->
```bash title="linux"
./java/Bingo/run_sample.sh
./java/DeliveryDispatch/run_sample.sh
./java/GameQuest/run_sample.sh
./java/ShoppingMall/run_sample.sh
./java/SupportChat/run_sample.sh
./java/TicTacToe/run_sample.sh
./java/ZoneWorld/run_sample.sh
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
```bash title="linux"
./kotlin/Bingo/run_sample.sh
./kotlin/DeliveryDispatch/run_sample.sh
./kotlin/GameQuest/run_sample.sh
./kotlin/ShoppingMall/run_sample.sh
./kotlin/SupportChat/run_sample.sh
./kotlin/TicTacToe/run_sample.sh
./kotlin/ZoneWorld/run_sample.sh
```
<!-- zlink-lang: end -->

Windows:

**Windows — PowerShell 7**

<!-- zlink-lang: java -->
```powershell title="windows"
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\Bingo\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\DeliveryDispatch\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\GameQuest\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\ShoppingMall\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\SupportChat\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\TicTacToe\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\java\ZoneWorld\run_sample.ps1
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
```powershell title="windows"
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\Bingo\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\DeliveryDispatch\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\GameQuest\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\ShoppingMall\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\SupportChat\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\TicTacToe\run_sample.ps1
pwsh -NoProfile -ExecutionPolicy Bypass -File .\kotlin\ZoneWorld\run_sample.ps1
```
<!-- zlink-lang: end -->

## Verify

Examples smoke runs this block exactly as written.

Each sample runner starts role-specific Spring processes, waits for readiness,
runs the probe or client scenario, and removes the processes and Redis
container it created. Application role code starts only its own role. After the
probe completes, the runner allows up to 90 seconds to observe the runtime's
30-second drain deadline and its bounded owner/resource cleanup before using
SIGKILL. A Framework process that reached `ZLINK_FRAMEWORK_READY` must also
write `ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE`; a missing
marker, a non-`STOPPED/NONE` result, a force kill, or cleanup failure makes the
sample fail. Exit code `0` with no further output beyond the run's own logs is
success.

## Troubleshooting

- **`No matching toolchain found`.** No JDK 25 on this machine. Follow the
  Temurin 25 install steps in Prerequisites.
- **`Connection refused` (Docker).** Docker Desktop isn't running. Start it
  and run again.
- **`No bindable ... Redis host port remained` or a sample port conflict.**
  Two samples of the same language are running at once, or a previous run
  didn't clean up. Each language uses its own port range (see
  `zlink_sample_configure_port_pool` in `gradle/zlink-jvm-runtime.sh`), so
  don't run two same-language samples concurrently.
- **`UnsupportedClassVersionError`.** `JAVA_HOME` at run time is older than
  JDK 25. The runner tries to find and set JDK 25 itself (see
  Prerequisites); if that search fails, install JDK 25 and point
  `JAVA_HOME` at it.
- **No `입력 파일이 너무 깁니다` / "input line is too long" regardless of
  project path depth.** `installDist`'s launcher scripts use a `lib`
  directory wildcard for the classpath instead of listing every jar, so
  they never hit Windows cmd.exe's 8191-character line limit.

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

Each language directory contains these seven sample roots. Its internal file layout may differ.

<!-- zlink-lang: java -->
```text
samples/java/
|-- Bingo/
|-- DeliveryDispatch/
|-- GameQuest/
|-- ShoppingMall/
|-- SupportChat/
|-- TicTacToe/
`-- ZoneWorld/
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
```text
samples/kotlin/
|-- Bingo/
|-- DeliveryDispatch/
|-- GameQuest/
|-- ShoppingMall/
|-- SupportChat/
|-- TicTacToe/
`-- ZoneWorld/
```
<!-- zlink-lang: end -->

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
