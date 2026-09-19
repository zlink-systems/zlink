# ZLink Java/Kotlin Tutorial

A step-by-step program that layers Channel messaging and one id-addressed
Spot. Both Java and Kotlin are included, as `java/` and `kotlin/`
subprojects of the same Gradle build, sharing `settings.gradle.kts`,
`gradle/libs.versions.toml`, and the wrapper.

한국어: [`README.ko.md`](./README.ko.md)

## Prerequisites

- **JDK 25.** No auto-download toolchain resolver — install it yourself and
  point `JAVA_HOME` at it. Details and exact error text:
  [`java/README.md`](./java/README.md#prerequisites).
- **Docker Desktop.** Runs Redis as one container.

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

## Download and install

No repository checkout. Build against the `systems.zlink:zlink-framework-*`
packages from Maven Central. The directory this zip extracts to
(`zlink-tutorial-java/`) is everything you need.

All commands below run from inside that directory.

## Build

```bash title="linux"
./gradlew :java:Server:installDist :java:Client:installDist
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist
```

```powershell title="windows"
.\gradlew.bat :java:Server:installDist :java:Client:installDist
.\gradlew.bat :kotlin:Server:installDist :kotlin:Client:installDist
```

## Run

Split by language. Both start the Server first. The blocks below start Java in the
background from one terminal (for Kotlin, replace `java` in the paths with `kotlin`).
Running them in the foreground in two terminals is described in each language README.

- Java: [`java/README.md`](./java/README.md#run)
- Kotlin: [`kotlin/README.md`](./kotlin/README.md#run)

```bash title="linux"
./java/Server/build/install/Server/bin/Server > server.log 2>&1 &
./java/Client/build/install/Client/bin/Client > client.log 2>&1 &
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5280/players/p1/profile >/dev/null && break; sleep 1; done
```

```powershell title="windows"
Start-Process -FilePath (Resolve-Path '.\java\Server\build\install\Server\bin\Server.bat') -WindowStyle Hidden -RedirectStandardOutput server.log -RedirectStandardError server.err.log
Start-Process -FilePath (Resolve-Path '.\java\Client\build\install\Client\bin\Client.bat') -WindowStyle Hidden -RedirectStandardOutput client.log -RedirectStandardError client.err.log
foreach ($i in 1..60) { try { Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile' -TimeoutSec 2 | Out-Null; break } catch { Start-Sleep -Seconds 1 } }
```

## Verify

Once the two processes accept each other, the `PEER_READY` log line described in each
language README's "Verify" appears, and the call below returns `200` with the profile.

```bash title="linux"
curl -sf http://127.0.0.1:5280/players/p1/profile | grep -q '"playerId":"p1"'
```

```powershell title="windows"
$profile = Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile'
if ($profile.playerId -ne 'p1') { throw "unexpected profile: $($profile | ConvertTo-Json -Compress)" }
```

## Troubleshooting

Ports and key prefixes differ per language, so this is split too. Docker/Redis
and JDK errors (connection refused, `UnsupportedClassVersionError`,
`No matching toolchain found`) and stale-Redis-key cleanup are each in
[`java/README.md`](./java/README.md#troubleshooting) and
[`kotlin/README.md`](./kotlin/README.md#troubleshooting).
