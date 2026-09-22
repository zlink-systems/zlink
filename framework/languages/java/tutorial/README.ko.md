# ZLink Java/Kotlin Tutorial

Channel 메시징과 id로 호출하는 Spot을 기능별로 추가하는 프로그램이다. Java와 Kotlin을
담았고, `java/`와 `kotlin/`은 같은 Gradle 빌드의 하위 프로젝트다.
`settings.gradle.kts`·`gradle/libs.versions.toml`·wrapper는 두 언어가 함께 쓴다.

English: [`README.md`](./README.md)

## 전제 조건

bash 블록은 Linux·macOS·WSL에서, PowerShell 블록은 Windows PowerShell 7에서 실행한다. `cmd`는 지원하지 않는다.

- **JDK 25.** 자동 toolchain resolver 없음 — 없으면 설치하고 `JAVA_HOME`을 그 경로로
  둔다. 자세한 내용과 오류 메시지는 [`java/README.ko.md`](./java/README.ko.md#전제-조건)를
  본다.
- **Docker Desktop.** Redis container를 실행한다.

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

## 내려받기와 설치

`zlink-java-examples` 저장소의 `tutorial/`에서 실행한다. `systems.zlink:zlink-framework-*`
패키지를 Maven Central에서 받아 빌드한다.

이 파일과 명령은 `zlink-java-examples` 저장소를 clone한 뒤 `tutorial/`을 현재 위치로 두고
실행한다.

## 빌드

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

## 실행

실행 절차는 언어별로 나뉜다. 두 언어 모두 Server를 먼저 실행한다. 아래는 Java를 한 터미널에서
백그라운드 process로 실행하는 방식이다(Kotlin은 경로의 `java`를 `kotlin`으로 바꾼다). 별도 터미널에서
실행하는 방법은 각 언어 README에 있다.

- Java: [`java/README.ko.md`](./java/README.ko.md#실행)
- Kotlin: [`kotlin/README.ko.md`](./kotlin/README.ko.md#실행)

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

## 검증

examples-smoke는 이 블록을 그대로 실행한다.

두 process의 연결이 준비되면 각 언어 README의 "검증"에 있는 `PEER_READY` 로그 줄이
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

## IDE에서 실행

IntelliJ에서 `tutorial/`을 Gradle project로 연다. Gradle tool window에서 Java의
`:java:Server:installDist`와 `:java:Client:installDist`, 또는 Kotlin의
`:kotlin:Server:installDist`와 `:kotlin:Client:installDist`를 실행한다. Application 구성에서
Server를 먼저, Client를 다음으로 실행한다. Java main class는
`systems.zlink.tutorial.server.ServerApplication`과
`systems.zlink.tutorial.client.ClientApplication`이고, Kotlin은 각각
`systems.zlink.tutorial.server.ServerApplicationKt`와
`systems.zlink.tutorial.client.ClientApplicationKt`이다. 종료는 IDE의 Stop 버튼으로 한다.

## 문제 해결

언어마다 포트와 key prefix가 달라 문제 해결 항목도 다르다. Docker/Redis·JDK 관련 오류(연결 거부,
`UnsupportedClassVersionError`, `No matching toolchain found`)와 stale Redis 키 정리는
[`java/README.ko.md`](./java/README.ko.md#문제-해결)와
[`kotlin/README.ko.md`](./kotlin/README.ko.md#문제-해결)에 각각 있다.
