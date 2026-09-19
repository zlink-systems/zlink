# Kotlin Tutorial

Full walkthrough (all 11 stages, the Kotlin/Java surface split, and the
`--8<--` snippet markers the guide reads) is in
[`README.ko.md`](./README.ko.md). This file only covers the six sections the
release CI job runs verbatim: prerequisites, download and install, build,
run, verify, and troubleshooting.

Kotlin has no directory of its own in this repository; it lives next to the
Java sources, in the same `zlink-tutorial-java` zip, under `kotlin/`.

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

This tutorial never checks out the repository; it only references
`systems.zlink:zlink-framework-*` packages from Maven Central. `kotlin/`
builds standalone as long as it travels with `../` (the tutorial root)'s
`settings.gradle.kts`, `gradle/libs.versions.toml`, and wrapper. There is
nothing separate to download or install: the Gradle wrapper fetches Gradle,
and Gradle fetches the packages above from Maven Central.

All commands below run from the directory this zip extracts to
(`zlink-tutorial-java/`, the tutorial root — `../` from here).

## Build

```bash title="linux"
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist
```

```powershell title="windows"
.\gradlew.bat :kotlin:Server:installDist :kotlin:Client:installDist
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
  `zlink-tutorial-kotlin:` key prefix in the same Redis. Clear only that
  prefix and restart the Server — leave other languages' tutorial keys
  alone.

  ```bash
  redis-cli --scan --pattern 'zlink-tutorial-kotlin:*' | xargs -r redis-cli del
  ```
