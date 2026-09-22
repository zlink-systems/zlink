# ZLink Java/Kotlin Tutorial

A step-by-step program that layers Channel messaging and one id-addressed
Spot. Both Java and Kotlin are included, as `java/` and `kotlin/`
subprojects of the same Gradle build, sharing `settings.gradle.kts`,
`gradle/libs.versions.toml`, and the wrapper.

한국어: [`README.ko.md`](./README.ko.md)

## Prerequisites

Bash blocks run on Linux, macOS, and WSL; PowerShell blocks run on Windows PowerShell 7. `cmd` is not supported.

- **JDK 25.** No auto-download toolchain resolver — install it yourself and
  point `JAVA_HOME` at it. Details and exact error text:
  [`java/README.md`](./java/README.md#prerequisites).
- **Docker Desktop.** Runs Redis as one container.

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

## Download and install

Run from `tutorial/` in the `zlink-java-examples` repository. Build against the
`systems.zlink:zlink-framework-*` packages from Maven Central.

Clone the `zlink-java-examples` repository and run all commands below from its `tutorial/`
directory.

## Build

**Linux · macOS · WSL — bash**

```bash title="linux"
./gradlew :java:Server:installDist :java:Client:installDist
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist
```

**Windows — PowerShell 7**

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

**Linux · macOS · WSL — bash**

```bash title="linux"
./java/Server/build/install/Server/bin/Server > server.log 2>&1 &
echo $! > server.pid
./java/Client/build/install/Client/bin/Client > client.log 2>&1 &
echo $! > client.pid
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5280/players/p1/profile >/dev/null && break; sleep 1; done
```

**Windows — PowerShell 7**

```powershell title="windows"
$server = Start-Process -FilePath (Resolve-Path '.\java\Server\build\install\Server\bin\Server.bat') -WindowStyle Hidden -RedirectStandardOutput server.log -RedirectStandardError server.err.log -PassThru
$server.Id | Set-Content server.pid
$client = Start-Process -FilePath (Resolve-Path '.\java\Client\build\install\Client\bin\Client.bat') -WindowStyle Hidden -RedirectStandardOutput client.log -RedirectStandardError client.err.log -PassThru
$client.Id | Set-Content client.pid
foreach ($i in 1..60) { try { Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile' -TimeoutSec 2 | Out-Null; break } catch { Start-Sleep -Seconds 1 } }
```

## Verify

Examples smoke runs this block exactly as written.

Once the two processes accept each other, the `PEER_READY` log line described in each
language README's "Verify" appears, and the call below returns `200` with the profile.

**Linux · macOS · WSL — bash**

```bash title="linux"
curl -sf http://127.0.0.1:5280/players/p1/profile | grep -q '"playerId":"p1"'
echo "tutorial-http=ok"
```

**Windows — PowerShell 7**

```powershell title="windows"
$playerProfile = Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile'
if ($playerProfile.playerId -ne 'p1') { throw "unexpected profile: $($playerProfile | ConvertTo-Json -Compress)" }
Write-Output 'tutorial-http=ok'
```

## Stop

Stop the processes started by the Run section.

**Linux · macOS · WSL — bash**

```bash title="linux"
for pid in "$(cat client.pid)" "$(cat server.pid)"; do
  pkill -TERM -P "$pid" 2>/dev/null || true
  kill "$pid" 2>/dev/null || true
done
```

**Windows — PowerShell 7**

```powershell title="windows"
Get-Content client.pid, server.pid | ForEach-Object {
  if ($_ -match '^\d+$') { taskkill /PID $_ /T /F 2>$null | Out-Null }
}
Get-Job | Stop-Job -ErrorAction SilentlyContinue
```

## Running from an IDE

Open `tutorial/` as a Gradle project in IntelliJ. In the Gradle tool window, run Java's
`:java:Server:installDist` and `:java:Client:installDist`, or Kotlin's
`:kotlin:Server:installDist` and `:kotlin:Client:installDist`. Use Application configurations to
run Server first and Client second. The Java main classes are
`systems.zlink.tutorial.server.ServerApplication` and
`systems.zlink.tutorial.client.ClientApplication`; Kotlin uses
`systems.zlink.tutorial.server.ServerApplicationKt` and
`systems.zlink.tutorial.client.ClientApplicationKt`. Stop with the IDE's Stop button.

## Troubleshooting

Ports and key prefixes differ per language, so this is split too. Docker/Redis
and JDK errors (connection refused, `UnsupportedClassVersionError`,
`No matching toolchain found`) and stale-Redis-key cleanup are each in
[`java/README.md`](./java/README.md#troubleshooting) and
[`kotlin/README.md`](./kotlin/README.md#troubleshooting).
