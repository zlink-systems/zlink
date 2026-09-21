[English](./README.md) | [한국어](./README.ko.md)

# ZLink Java/Kotlin quickstart

This is the smallest project: two processes call each other once over a channel, with no
location store because each client names the server endpoint directly. The site pages
`framework/doc/framework/java/quickstart.ko.md` and
`framework/doc/framework/kotlin/quickstart.ko.md` read their code blocks from these files.
This directory is `quickstart/` in the `zlink-java-examples` repository, with `java/` and
`kotlin/` subprojects in one Gradle build.

| | Purpose |
|---|---|
| **quickstart** (here) | Installs packages and reaches the first reply |
| tutorial (`tutorial/`) | Adds features one at a time. The feature guides read this code |
| samples (`samples/`) | Shows applications with a complete business flow |

## Prerequisites

- JDK 25 or newer. The Java and Kotlin subprojects pin their toolchains to 25.
- The Gradle wrapper included in this directory. It downloads Gradle and the packages from
  Maven Central on the first build.
- No Redis or other external service.

## Download and install

Clone the [`zlink-java-examples`](https://github.com/zlink-systems/zlink-java-examples)
repository. Run the commands below from its `quickstart/` directory.

`gradle/libs.versions.toml` pins `zlink-framework-core`, `zlink-framework-spring-boot-starter`,
`zlink-framework-kotlin`, and the Spring Boot, Kotlin and Jackson versions.
The `systems.zlink:zlink` binding is not listed; it resolves transitively from
`zlink-framework-core`.

The Kotlin Client also declares `kotlinx-coroutines-reactor` without a version in its
subproject build file. Its version is resolved by the transitive coroutine constraints.

## Build

```bash title="linux"
./gradlew \
  :java:Server:installDist :java:Client:installDist \
  :kotlin:Server:installDist :kotlin:Client:installDist
```

```powershell title="windows"
.\gradlew.bat `
  :java:Server:installDist :java:Client:installDist `
  :kotlin:Server:installDist :kotlin:Client:installDist
```

## Run

Run one language pair at a time. Each Server listens on `tcp://0.0.0.0:7101` and handles the
`greeting` channel. Each Client listens on `tcp://0.0.0.0:7102`, connects to
`tcp://127.0.0.1:7101`, and serves `GET /hello/{name}` on `http://127.0.0.1:5080`.

```bash title="linux"
./java/Server/build/install/Server/bin/Server > server.log 2>&1 &
./java/Client/build/install/Client/bin/Client > client.log 2>&1 &
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5080/hello/world > /dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:5080/hello/world
```

```powershell title="windows"
Start-Process -NoNewWindow .\java\Server\build\install\Server\bin\Server.bat -RedirectStandardOutput server.log -RedirectStandardError server.err.log
Start-Process -NoNewWindow .\java\Client\build\install\Client\bin\Client.bat -RedirectStandardOutput client.log -RedirectStandardError client.err.log
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5080/hello/world; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
if ($LASTEXITCODE -ne 0) { throw 'quickstart did not come up' }
$answer
```

For the Kotlin pair, stop both processes and run the same commands under `kotlin/`
(`./kotlin/Server/build/install/Server/bin/Server`, `.../Client`).

## Verify

```bash title="linux"
set -e
curl -sf http://127.0.0.1:5080/hello/world | grep -q 'hello, world'
echo "quickstart=ok"
```

```powershell title="windows"
$answer = curl.exe -sf http://127.0.0.1:5080/hello/world
if ($LASTEXITCODE -ne 0 -or $answer -notmatch 'hello, world') { throw 'quickstart failed' }
Write-Output 'quickstart=ok'
```

The endpoint returns `hello, world` with HTTP status 200 for both the Java and Kotlin pairs.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Ports 7101, 7102, or 5080 are busy | Stop the earlier language pair |
| The curl request cannot connect | Start Server, then Client, and inspect process output |
| The request has no target | Match `connect` and `listen` endpoints |
| The Server exits after startup | Keep `setKeepAlive(true)` in each non-web Server |
| Java Client returns a path-variable error | Keep `-parameters` in Java Client compiler options |
| Kotlin coroutine request fails | Keep `kotlinx-coroutines-reactor` in Client |
| Kotlin startup or decode fails | Keep the Kotlin Spring plugin and `jackson-module-kotlin` |

## Project layout

| Path | Contents |
|---|---|
| `java/Shared`, `kotlin/Shared` | Java record and Kotlin data-class contracts |
| `java/Server`, `kotlin/Server` | Register the `greeting` handler and listen on port 7101 |
| `java/Client`, `kotlin/Client` | Connect and expose `GET /hello/{name}` on port 5080 |
| `gradle/libs.versions.toml` | Central package and plugin pins |
| `gradlew`, `gradlew.bat` | Gradle wrapper launchers for Linux and Windows |

## What to carry into your own project

- `gradle/libs.versions.toml`, including the explicit framework, Spring Boot, Kotlin, and
  Jackson versions. Leave `systems.zlink:zlink` to the framework's transitive dependency.
- The Java record and Kotlin data-class contract shapes in `Shared`.
- The Server `ZLinkFrameworkConfigurer` block: mesh name, `listen(...)`,
  `channelName(...).server().addRequestHandler(...)`, and `setKeepAlive(true)` for a
  non-web process.
- The Client block: `channelName(...).client()`, `peerConnections().connect(...)`, and the
  `ZLinkRouteClient` call site.
- A production service normally replaces the manual peer connection with a location store,
  such as Redis. This quickstart omits that service dependency.
