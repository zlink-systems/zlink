<!-- zlink-lang: java -->
# ZLink Java Tutorial

Java subproject와 Gradle root 파일을 담는다.
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
# ZLink Kotlin Tutorial

Kotlin subproject와 Gradle root 파일을 담는다.
<!-- zlink-lang: end -->

Channel 메시징과 id로 호출하는 Spot을 기능별로 추가하는 프로그램이다.

English: [`README.md`](./README.md)

## 전제 조건

bash 블록은 Linux·macOS·WSL에서, PowerShell 블록은 Windows PowerShell 7에서 실행한다. `cmd`는 지원하지 않는다.

- **JDK 25.** 자동 toolchain resolver 없음 — 없으면 설치하고 `JAVA_HOME`을 그 경로로 둔다.
<!-- zlink-lang: java -->
  자세한 내용과 오류 메시지는 [`java/README.ko.md`](./java/README.ko.md#전제-조건)를 본다.
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
  자세한 내용과 오류 메시지는 [`kotlin/README.ko.md`](./kotlin/README.ko.md#전제-조건)를 본다.
<!-- zlink-lang: end -->
- **Docker Desktop.** Redis container를 실행한다.

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

## 내려받기와 설치

<!-- zlink-lang: java -->
`zlink-java-examples` 저장소의 `tutorial/`에서 실행한다.
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
`zlink-kotlin-examples` 저장소의 `tutorial/`에서 실행한다.
<!-- zlink-lang: end -->
`systems.zlink:zlink-framework-*`
패키지를 Maven Central에서 받아 빌드한다.

이 파일과 명령은 해당 examples 저장소를 clone한 뒤 `tutorial/`을 현재 위치로 두고 실행한다.

## 빌드

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

## 실행

Server를 먼저 실행한다. 별도 터미널에서 실행하는 방법은 language README에 있다.

<!-- zlink-lang: java -->
- [`java/README.ko.md`](./java/README.ko.md#실행)
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
- [`kotlin/README.ko.md`](./kotlin/README.ko.md#실행)
<!-- zlink-lang: end -->

**Linux · macOS · WSL — bash**

<!-- zlink-lang: java -->
```bash title="linux"
./java/Server/build/install/Server/bin/Server > server.log 2>&1 &
echo $! > server.pid
./java/Client/build/install/Client/bin/Client > client.log 2>&1 &
echo $! > client.pid
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5280/players/p1/profile >/dev/null && break; sleep 1; done
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
```bash title="linux"
./kotlin/Server/build/install/Server/bin/Server > server.log 2>&1 &
echo $! > server.pid
./kotlin/Client/build/install/Client/bin/Client > client.log 2>&1 &
echo $! > client.pid
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5280/players/p1/profile >/dev/null && break; sleep 1; done
```
<!-- zlink-lang: end -->

**Windows — PowerShell 7**

<!-- zlink-lang: java -->
```powershell title="windows"
$server = Start-Process -FilePath (Resolve-Path '.\java\Server\build\install\Server\bin\Server.bat') -WindowStyle Hidden -RedirectStandardOutput server.log -RedirectStandardError server.err.log -PassThru
$server.Id | Set-Content server.pid
$client = Start-Process -FilePath (Resolve-Path '.\java\Client\build\install\Client\bin\Client.bat') -WindowStyle Hidden -RedirectStandardOutput client.log -RedirectStandardError client.err.log -PassThru
$client.Id | Set-Content client.pid
foreach ($i in 1..60) { try { Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile' -TimeoutSec 2 | Out-Null; break } catch { Start-Sleep -Seconds 1 } }
```
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
```powershell title="windows"
$server = Start-Process -FilePath (Resolve-Path '.\kotlin\Server\build\install\Server\bin\Server.bat') -WindowStyle Hidden -RedirectStandardOutput server.log -RedirectStandardError server.err.log -PassThru
$server.Id | Set-Content server.pid
$client = Start-Process -FilePath (Resolve-Path '.\kotlin\Client\build\install\Client\bin\Client.bat') -WindowStyle Hidden -RedirectStandardOutput client.log -RedirectStandardError client.err.log -PassThru
$client.Id | Set-Content client.pid
foreach ($i in 1..60) { try { Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile' -TimeoutSec 2 | Out-Null; break } catch { Start-Sleep -Seconds 1 } }
```
<!-- zlink-lang: end -->

## 검증

examples-smoke는 이 블록을 그대로 실행한다.

두 process의 연결이 준비되면 language README의 "검증"에 있는 `PEER_READY` 로그 줄이
기록되고, 아래 호출은 `200`과 함께 profile을 반환한다.

**Linux · macOS · WSL — bash**

```bash title="linux"
curl -sf http://127.0.0.1:5280/players/p1/profile | grep -q '"playerId":"p1"'
echo "tutorial-http=ok"
```

**Windows — PowerShell 7**

```powershell title="windows"
$profile = Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile'
if ($profile.playerId -ne 'p1') { throw "unexpected profile: $($profile | ConvertTo-Json -Compress)" }
Write-Output 'tutorial-http=ok'
```

## 종료

실행 절에서 시작한 process를 종료한다.

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

## 문제 해결

언어마다 포트와 key prefix가 달라 문제 해결 항목도 다르다. Docker/Redis·JDK 관련 오류(연결 거부,
`UnsupportedClassVersionError`, `No matching toolchain found`)와 stale Redis 키 정리는 아래 language
README에 있다.

<!-- zlink-lang: java -->
[`java/README.ko.md`](./java/README.ko.md#문제-해결)
<!-- zlink-lang: end -->
<!-- zlink-lang: kotlin -->
[`kotlin/README.ko.md`](./kotlin/README.ko.md#문제-해결)
<!-- zlink-lang: end -->
