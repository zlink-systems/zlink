# Java Tutorial — Channel Messaging And Spot

Full walkthrough (all 12 stages, Java ↔ .NET surface differences, and the
`--8<--` snippet markers the guide reads) is in
[`README.ko.md`](./README.ko.md). This file only covers the six sections the
release CI job runs verbatim: prerequisites, download and install, build,
run, verify, and troubleshooting.

The Kotlin tutorial lives next to this one, in the same Gradle build
(`../kotlin/`). Both share `settings.gradle.kts`, `gradle/libs.versions.toml`,
and the wrapper.

## Prerequisites

- **JDK 25.** The published `zlink-framework-*` packages (version: see
  `zlinkFramework` in [`../gradle/libs.versions.toml`](../gradle/libs.versions.toml))
  have class file major version 69. A lower JVM refuses to load them with
  `UnsupportedClassVersionError`. The Gradle toolchain is pinned to 25, and
  **the `installDist` launcher scripts use whatever `JAVA_HOME` is set at run
  time, so it must also be JDK 25.**

  This Gradle project does not configure a JDK auto-download toolchain
  resolver (such as `org.gradle.toolchains.foojay-resolver-convention`). If
  JDK 25 is missing, install [Temurin 25](https://adoptium.net/) and point
  `JAVA_HOME` at it (both Gradle and the `installDist` launcher scripts read
  `JAVA_HOME`).

  ```bash
  # Linux/WSL
  export JAVA_HOME=/path/to/jdk-25.0.4.1+1
  ```

  ```powershell
  # Windows PowerShell
  $env:JAVA_HOME = "C:\path\to\jdk-25.0.4.1+1"
  ```

- **Docker Desktop.** Redis runs as a single Docker container, no repository
  checkout needed (start it before "Build" below).

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

Nothing else is required. `zlink-framework-core`'s POM points at binding
`systems.zlink:zlink`, and that jar bundles both Linux and Windows native
libraries (`LibraryLoader` finds and loads them from inside the jar), so no
extra native setup is needed on Windows either.

## Download and install

This tutorial runs from `tutorial/java/` in the `zlink-java-examples` repository and only
references `systems.zlink:zlink-framework-*` packages from Maven Central. This directory
builds standalone as long as it travels with `../`
(the tutorial root)'s `settings.gradle.kts`, `gradle/libs.versions.toml`,
and wrapper — which is why it sits next to the Kotlin tutorial in the same
repository. There is nothing separate to download or install: the Gradle wrapper
(`./gradlew` / `gradlew.bat`) fetches the right Gradle version on first run,
and Gradle then fetches the packages above from Maven Central.

Clone the `zlink-java-examples` repository and run all commands below from its tutorial root;
`../` from here is the `tutorial/java/` project.

## Build

```bash title="linux"
./gradlew :java:Server:installDist :java:Client:installDist :java:HttpClient:installDist
```

```powershell title="windows"
.\gradlew.bat :java:Server:installDist :java:Client:installDist :java:HttpClient:installDist
```

## Run

Two terminals. Start the Server first.

```bash title="linux"
./java/Server/build/install/Server/bin/Server
./java/Client/build/install/Client/bin/Client
```

```powershell title="windows"
.\java\Server\build\install\Server\bin\Server.bat
.\java\Client\build\install\Client\bin\Client.bat
```

The STREAM stage's external client is a third subproject.

```bash
./gradlew :java:StreamClient:installDist
./java/StreamClient/build/install/StreamClient/bin/StreamClient
```

The HTTP client is a separate process outside the mesh. It references only the
published `zlink-http-client` package and exercises the Client HTTP surface and
the authenticated Server admin surface.

```bash title="linux"
./gradlew :java:HttpClient:installDist
./java/HttpClient/build/install/HttpClient/bin/HttpClient
```

```powershell title="windows"
.\gradlew.bat :java:HttpClient:installDist
.\java\HttpClient\build\install\HttpClient\bin\HttpClient.bat
```

```text
first request: p1 rookie
request shaping: status 200 weight 2
json body: player 200 room 8ed46dd8-e11d-40fd-8e95-c7be5eda90bc chat 202
response kinds: typed 200 raw application/json fetch anonymous
compressed response: 200 encoding-removed true
redirect: 200 p1
basic auth: without 401 with 200
download stream: chunks 2 bytes 132
upload stream: imported 3
error kinds: bad request INTERNAL_FAILURE connection refused UNAVAILABLE
```

## HTTP operational surface

The Server admin route requires `ops:tutorial-admin` and returns `401` with
`WWW-Authenticate: Basic realm="tutorial-admin"` when the credentials are absent.
The Client serves gzip when `Accept-Encoding: gzip` is present, redirects
`/player/p1` to `/players/p1`, and exposes chunked NDJSON export/import at
`/rooms/<roomId>/export` and `/rooms/<roomId>/import`.

```bash
curl -i -u ops:tutorial-admin -X POST \
  'http://127.0.0.1:5281/admin/channels/profile/weight?value=2'
# 200
# {"channel":"profile","weight":2}

curl -i -H 'Accept-Encoding: gzip' http://127.0.0.1:5280/rooms/<roomId>
# 200, Content-Encoding: gzip

curl -i http://127.0.0.1:5280/player/p1
# 301, Location: /players/p1

curl -i http://127.0.0.1:5280/rooms/<roomId>/export
# 200, Transfer-Encoding: chunked, Content-Type: application/x-ndjson

curl -i -X POST http://127.0.0.1:5280/rooms/<roomId>/import \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary $'{"playerId":"p2","text":"one"}\n{"playerId":"p2","text":"two"}\n'
# 200, {"imported":2}
```

## Snippet markers

| Marker | Source |
|---|---|
| `http-client-create` | `HttpClient/.../HttpClientProgram.java` |
| `http-first-request` | `HttpClient/.../HttpClientProgram.java` |
| `http-request-shaping` | `HttpClient/.../HttpClientProgram.java` |
| `http-json-body` | `HttpClient/.../HttpClientProgram.java` |
| `http-response-kinds` | `HttpClient/.../HttpClientProgram.java` |
| `http-compressed-response` | `HttpClient/.../HttpClientProgram.java` |
| `http-redirect` | `HttpClient/.../HttpClientProgram.java` |
| `http-basic-auth` | `HttpClient/.../HttpClientProgram.java` |
| `http-download-stream` | `HttpClient/.../HttpClientProgram.java` |
| `http-upload-stream` | `HttpClient/.../HttpClientProgram.java` |
| `http-error-kinds` | `HttpClient/.../HttpClientProgram.java` |

## Verify

Once the two processes accept each other, one line appears in each log.

```
s.z.f.r.binding.ZLinkJavaRawMeshNode : ZLINK_FRAMEWORK_PEER_READY mesh=game peer=game-server-1
s.z.f.r.binding.ZLinkJavaRawMeshNode : ZLINK_FRAMEWORK_PEER_READY mesh=game peer=game-ee30c320-1220-452c-b1ea-22546350096c
```

With the Client up, this call returning `200` with a profile confirms
success (see stage 1 in the Korean README's walkthrough for more calls).

```bash title="linux"
curl http://127.0.0.1:5280/players/p1/profile
# 200
# {"playerId":"p1","nickname":"rookie","level":1}
```

```powershell title="windows"
Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile'
```

## Troubleshooting

- **`UnsupportedClassVersionError`.**

  ```
  UnsupportedClassVersionError: systems/zlink/tutorial/server/ServerApplication
  has been compiled by a more recent version of the Java Runtime
  (class file version 69.0), this version of the Java Runtime only recognizes
  class file versions up to 66.0
  ```

  `JAVA_HOME` at run time is older than JDK 25. Point it at JDK 25 as
  described in Prerequisites.

- **`No matching toolchain found for requested specification:
  {languageVersion=25, ...}`.** The build machine has no JDK 25 at all.
  Follow the Temurin 25 install steps in Prerequisites.

- **`Connection refused` (Redis, 6379).** Docker isn't running, or the
  container is still starting. Keep `docker run --rm -p 6379:6379 redis`
  running in another terminal until its log shows
  `Ready to accept connections`, then run the tutorial.

- **`Address already in use`
  (5280/5281/7501/7502/7511/7512/7521).** A previous run is still up. Stop
  both processes and run again.

- **`ZLinkConfigurationException: MeshNode descriptor publication failed
  [mesh=game, status=REJECTED_CONFLICT]`, or the profile call keeps
  returning `503 one-way route is not connected`.** This happens when
  another run left a mesh descriptor behind under the `zlink-tutorial-java:`
  key prefix in the same Redis. Clear only that prefix and restart the
  Server — leave other languages' tutorial keys (`zlink-tutorial-dotnet:`,
  etc.) alone.

  ```bash
  redis-cli --scan --pattern 'zlink-tutorial-java:*' | xargs -r redis-cli del
  ```
