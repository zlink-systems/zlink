한국어: [`README.ko.md`](./README.ko.md)

<!-- zlink-lang: java -->
# ZLink Java Tutorial

Contains Java subprojects and the shared Gradle root files.
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
# ZLink Kotlin Tutorial

Contains Kotlin subprojects and the shared Gradle root files.
<!-- zlink-lang: end -->

A step-by-step program that layers Channel messaging and one id-addressed Spot.

## Prerequisites

Bash blocks run on Linux, macOS, and WSL; PowerShell blocks run on Windows PowerShell 7. `cmd` is not supported.

- **JDK 25.** No auto-download toolchain resolver — install it yourself and point `JAVA_HOME` at it.
<!-- zlink-lang: java -->
  Details and exact error text: [`java/README.md`](./java/README.md#prerequisites).
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
  Details and exact error text: [`kotlin/README.md`](./kotlin/README.md#prerequisites).
<!-- zlink-lang: end -->
- **Docker Desktop.** Runs Redis as one container.

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

## Download and install

<!-- zlink-lang: java -->
Run from `tutorial/` in `zlink-java-examples`.
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
Run from `tutorial/` in `zlink-kotlin-examples`.
<!-- zlink-lang: end -->
Build against the `systems.zlink:zlink-framework-*` packages from Maven Central.

## Build

**Linux · macOS · WSL — bash**

<!-- zlink-lang: java -->
```bash title="linux"
./gradlew :java:Server:installDist :java:Client:installDist
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
```bash title="linux"
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist
```
<!-- zlink-lang: end -->

**Windows — PowerShell 7**

<!-- zlink-lang: java -->
```powershell title="windows"
.\gradlew.bat :java:Server:installDist :java:Client:installDist
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
```powershell title="windows"
.\gradlew.bat :kotlin:Server:installDist :kotlin:Client:installDist
```
<!-- zlink-lang: end -->

## Run

Start Server first. The language README describes the foreground two-terminal procedure.

<!-- zlink-lang: java -->
[`java/README.md`](./java/README.md#run)

```bash title="linux"
./java/Server/build/install/Server/bin/Server > server.log 2>&1 &
echo $! > server.pid
./java/Client/build/install/Client/bin/Client > client.log 2>&1 &
echo $! > client.pid
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5280/players/p1/profile >/dev/null && break; sleep 1; done
```

```powershell title="windows"
$server = Start-Process -FilePath (Resolve-Path '.\java\Server\build\install\Server\bin\Server.bat') -WindowStyle Hidden -RedirectStandardOutput server.log -RedirectStandardError server.err.log -PassThru
$server.Id | Set-Content server.pid
$client = Start-Process -FilePath (Resolve-Path '.\java\Client\build\install\Client\bin\Client.bat') -WindowStyle Hidden -RedirectStandardOutput client.log -RedirectStandardError client.err.log -PassThru
$client.Id | Set-Content client.pid
foreach ($i in 1..60) { try { Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile' -TimeoutSec 2 | Out-Null; break } catch { Start-Sleep -Seconds 1 } }
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
[`kotlin/README.md`](./kotlin/README.md#run)

```bash title="linux"
./kotlin/Server/build/install/Server/bin/Server > server.log 2>&1 &
echo $! > server.pid
./kotlin/Client/build/install/Client/bin/Client > client.log 2>&1 &
echo $! > client.pid
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5380/players/p1/profile >/dev/null && break; sleep 1; done
```

```powershell title="windows"
$server = Start-Process -FilePath (Resolve-Path '.\kotlin\Server\build\install\Server\bin\Server.bat') -WindowStyle Hidden -RedirectStandardOutput server.log -RedirectStandardError server.err.log -PassThru
$server.Id | Set-Content server.pid
$client = Start-Process -FilePath (Resolve-Path '.\kotlin\Client\build\install\Client\bin\Client.bat') -WindowStyle Hidden -RedirectStandardOutput client.log -RedirectStandardError client.err.log -PassThru
$client.Id | Set-Content client.pid
foreach ($i in 1..60) { try { Invoke-RestMethod -Uri 'http://127.0.0.1:5380/players/p1/profile' -TimeoutSec 2 | Out-Null; break } catch { Start-Sleep -Seconds 1 } }
```
<!-- zlink-lang: end -->

## Verify

Examples smoke runs this block exactly as written. Once the processes connect, `PEER_READY` is recorded and the call returns `200` with the profile.

<!-- zlink-lang: java -->
```bash title="linux"
curl -sf http://127.0.0.1:5280/players/p1/profile | grep -q '"playerId":"p1"'
echo "tutorial-http=ok"
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
```bash title="linux"
curl -sf http://127.0.0.1:5380/players/p1/profile | grep -q '"playerId":"p1"'
echo "tutorial-http=ok"
```
<!-- zlink-lang: end -->

<!-- zlink-lang: java -->
```powershell title="windows"
$playerProfile = Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile'
if ($playerProfile.playerId -ne 'p1') { throw "unexpected profile: $($playerProfile | ConvertTo-Json -Compress)" }
Write-Output 'tutorial-http=ok'
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
```powershell title="windows"
$playerProfile = Invoke-RestMethod -Uri 'http://127.0.0.1:5380/players/p1/profile'
if ($playerProfile.playerId -ne 'p1') { throw "unexpected profile: $($playerProfile | ConvertTo-Json -Compress)" }
Write-Output 'tutorial-http=ok'
```
<!-- zlink-lang: end -->

## Stop

```bash title="linux"
for pid in "$(cat client.pid)" "$(cat server.pid)"; do
  pkill -TERM -P "$pid" 2>/dev/null || true
  kill "$pid" 2>/dev/null || true
done
```

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

Docker/Redis, JDK, and stale-key troubleshooting:

<!-- zlink-lang: java -->
[`java/README.md`](./java/README.md#troubleshooting)
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
[`kotlin/README.md`](./kotlin/README.md#troubleshooting)
<!-- zlink-lang: end -->
