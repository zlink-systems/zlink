# Kotlin Tutorial

Full walkthrough (all 12 stages, the Kotlin/Java surface split, and the
`--8<--` snippet markers the guide reads) is in
[`README.ko.md`](./README.ko.md). This file only covers the six sections the
release CI job runs verbatim: prerequisites, download and install, build,
run, verify, and troubleshooting.

Kotlin has no directory of its own in this repository; it lives next to the
Java sources in the `zlink-java-examples` repository, under `tutorial/kotlin/`.

## Prerequisites

- **JDK 25 or newer.** The published `zlink-framework-core` (version: see
  `zlinkFramework` in [`../gradle/libs.versions.toml`](../gradle/libs.versions.toml))
  has class file version 69 (Java 25). **The `installDist` launcher scripts
  use whatever `JAVA_HOME` is set at run time, so it must also be JDK 25.**

  This Gradle project does not configure a JDK auto-download toolchain
  resolver (such as `org.gradle.toolchains.foojay-resolver-convention`). If
  JDK 25 is missing, install [Temurin 25](https://adoptium.net/) and point
  `JAVA_HOME` at it.

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
libraries, so no extra native setup is needed on Windows either.

## Download and install

This tutorial runs from `tutorial/kotlin/` in the `zlink-java-examples` repository and only
references `systems.zlink:zlink-framework-*` packages from Maven Central. `kotlin/`
builds standalone as long as it travels with `../` (the tutorial root)'s
`settings.gradle.kts`, `gradle/libs.versions.toml`, and wrapper. There is
nothing separate to download or install: the Gradle wrapper fetches Gradle,
and Gradle fetches the packages above from Maven Central.

Clone the `zlink-java-examples` repository and run all commands below from its tutorial root;
`../` from here is the `tutorial/kotlin/` project.

## Build

```bash title="linux"
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist :kotlin:HttpClient:installDist
```

```powershell title="windows"
.\gradlew.bat :kotlin:Server:installDist :kotlin:Client:installDist :kotlin:HttpClient:installDist
```

## Run

Two terminals. Start the Server first.

```bash title="linux"
kotlin/Server/build/install/Server/bin/Server
kotlin/Client/build/install/Client/bin/Client
```

```powershell title="windows"
.\kotlin\Server\build\install\Server\bin\Server.bat
.\kotlin\Client\build\install\Client\bin\Client.bat
```

The STREAM stage's external client is a third subproject.

```bash
./gradlew :kotlin:StreamClient:installDist
kotlin/StreamClient/build/install/StreamClient/bin/StreamClient
```

The HTTP client is a separate process outside the mesh. It uses the Kotlin
`zlinkHttpClient { }` DSL and coroutine terminal extensions, and references only
the published `zlink-http-client-kotlin` wrapper.

```bash title="linux"
./gradlew :kotlin:HttpClient:installDist
kotlin/HttpClient/build/install/HttpClient/bin/HttpClient
```

```powershell title="windows"
.\gradlew.bat :kotlin:HttpClient:installDist
.\kotlin\HttpClient\build\install\HttpClient\bin\HttpClient.bat
```

```text
first request: p1 rookie
request shaping: status 200 weight 2
json body: player 200 room 47e7f688-6185-41b9-8e16-c22ef0dcd075 chat 202
response kinds: typed 200 raw application/json fetch speedy-p2
compressed response: 200 encoding-removed true
redirect: 200 p1
basic auth: without 401 with 200
download stream: chunks 2 bytes 132
upload stream: imported 3
error kinds: bad request INTERNAL_FAILURE connection refused UNAVAILABLE
```

## HTTP operational surface

The Server admin route requires `ops:tutorial-admin` and returns `401` with
`WWW-Authenticate: Basic realm="tutorial-admin"` when credentials are absent.
The Client serves gzip when `Accept-Encoding: gzip` is present, redirects
`/player/p1` to `/players/p1`, and exposes chunked NDJSON export/import at
`/rooms/<roomId>/export` and `/rooms/<roomId>/import`.

```bash
curl -i -u ops:tutorial-admin -X POST \
  'http://127.0.0.1:5381/admin/channels/profile/weight?value=2'
# 200
# {"channel":"profile","weight":2}

curl -i -H 'Accept-Encoding: gzip' http://127.0.0.1:5380/rooms/<roomId>
# 200, Content-Encoding: gzip

curl -i http://127.0.0.1:5380/player/p1
# 301, Location: /players/p1

curl -i http://127.0.0.1:5380/rooms/<roomId>/export
# 200, Transfer-Encoding: chunked, Content-Type: application/x-ndjson

curl -i -X POST http://127.0.0.1:5380/rooms/<roomId>/import \
  -H 'Content-Type: application/x-ndjson' \
  --data-binary $'{"playerId":"p2","text":"one"}\n{"playerId":"p2","text":"two"}\n'
# 200, {"imported":2}
```

## Snippet markers

| Marker | Source |
|---|---|
| `http-client-create` | `HttpClient/.../HttpClientProgram.kt` |
| `http-first-request` | `HttpClient/.../HttpClientProgram.kt` |
| `http-request-shaping` | `HttpClient/.../HttpClientProgram.kt` |
| `http-json-body` | `HttpClient/.../HttpClientProgram.kt` |
| `http-response-kinds` | `HttpClient/.../HttpClientProgram.kt` |
| `http-compressed-response` | `HttpClient/.../HttpClientProgram.kt` |
| `http-redirect` | `HttpClient/.../HttpClientProgram.kt` |
| `http-basic-auth` | `HttpClient/.../HttpClientProgram.kt` |
| `http-download-stream` | `HttpClient/.../HttpClientProgram.kt` |
| `http-upload-stream` | `HttpClient/.../HttpClientProgram.kt` |
| `http-error-kinds` | `HttpClient/.../HttpClientProgram.kt` |

## Verify

Once the Server accepts a peer, this line is printed.

```
INFO 38136 --- [m-raw-mesh-game] s.z.f.r.binding.ZLinkJavaRawMeshNode     : ZLINK_FRAMEWORK_PEER_READY mesh=game peer=game-1926e18d-0ff9-4114-8168-2587672b7865
```

With the Client up, this call returning `200` with a profile confirms
success (see stage 1 in the Korean README's walkthrough for more calls).

```bash title="linux"
curl http://127.0.0.1:5380/players/p1/profile
# 200
# {"playerId":"p1","nickname":"rookie","level":1}
```

```powershell title="windows"
Invoke-RestMethod -Uri 'http://127.0.0.1:5380/players/p1/profile'
```

## Troubleshooting

- **`UnsupportedClassVersionError`.** `JAVA_HOME` at run time is older than
  JDK 25. Point it at JDK 25 as described in Prerequisites.

- **`No matching toolchain found`.** The build machine has no JDK 25 at
  all. Follow the Temurin 25 install steps in Prerequisites.

- **`Connection refused` (Redis, 6379).** Docker isn't running, or the
  container is still starting. Keep `docker run --rm -p 6379:6379 redis`
  running in another terminal until its log shows
  `Ready to accept connections`, then run the tutorial.

- **Port 6379 is already in use by Redis.** If
  `redis-cli -h 127.0.0.1 -p 6379 ping` returns `PONG`, use that Redis and
  skip `docker run`. Stop any processes from a previous tutorial run, execute
  the `zlink-tutorial-kotlin:*` key cleanup command below, then restart the
  Server and Client. Do not stop the existing Redis when the tutorial ends.

- **`Address already in use`
  (5380/5381/7601/7602/7611/7612/7621).** A previous run is still up. Stop
  both processes and run again.

- **`java.lang.IllegalStateException: Missing native symbol 'zlink_publish'.
  Loaded libzlink is incompatible with this Java binding.`** Happens when
  `ZLINK_LIBRARY_PATH` points at a different version of `zlink.dll` /
  `libzlink.so`. Unset it and use the native bundled inside the jar.

- **`ZLinkConfigurationException: MeshNode descriptor publication failed
  [mesh=game, status=REJECTED_CONFLICT]`, or the profile call keeps
  returning `503 one-way route is not connected`.** This happens when
  another run left a mesh descriptor behind under the
  `zlink-tutorial-kotlin:` key prefix in the same Redis. Stop any previous
  tutorial processes, clear only that prefix, and
  restart the Server. Leave other languages' tutorial keys intact.

  ```bash
  redis-cli --scan --pattern 'zlink-tutorial-kotlin:*' | xargs -r redis-cli del
  ```

  On Windows, run this with `redis-cli` connected to the same Redis:

  ```powershell
  redis-cli --scan --pattern 'zlink-tutorial-kotlin:*' | ForEach-Object { redis-cli DEL $_ | Out-Null }
  ```
