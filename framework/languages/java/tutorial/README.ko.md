# ZLink Java/Kotlin Tutorial

Channel 메시징과 id로 부르는 Spot 하나를 기능별로 쌓아 가는 프로그램이다. Java와 Kotlin
두 언어를 담았고, `java/`와 `kotlin/`이 같은 Gradle build 안 subproject다.
`settings.gradle.kts`·`gradle/libs.versions.toml`·wrapper는 두 언어가 함께 쓴다.

English: [`README.md`](./README.md)

## 전제 조건

- **JDK 25.** 자동 toolchain resolver 없음 — 없으면 설치하고 `JAVA_HOME`을 그 경로로
  둔다. 자세한 내용과 오류 메시지는 [`java/README.ko.md`](./java/README.ko.md#전제-조건)를
  본다.
- **Docker Desktop.** Redis 하나를 컨테이너로 띄운다.

  ```bash
  docker run --rm -p 6379:6379 redis
  ```

## 내려받기와 설치

`zlink-java-examples` 저장소의 `tutorial/`에서 실행한다. `systems.zlink:zlink-framework-*`
패키지를 Maven Central에서 받아 빌드한다.

이 파일과 명령은 `zlink-java-examples` 저장소를 clone한 뒤 `tutorial/`을 현재 위치로 두고
실행한다.

## 빌드

```bash title="linux"
./gradlew :java:Server:installDist :java:Client:installDist
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist
```

```powershell title="windows"
.\gradlew.bat :java:Server:installDist :java:Client:installDist
.\gradlew.bat :kotlin:Server:installDist :kotlin:Client:installDist
```

## 실행

언어별로 나뉜다. Server를 먼저 실행하는 것은 둘 다 같다. 아래는 Java를 한 터미널에서
백그라운드로 띄우는 형태다(Kotlin은 경로의 `java`를 `kotlin`으로 바꾼다). 터미널 두 개로
나누어 전면 실행하는 방법은 각 언어 README에 있다.

- Java: [`java/README.ko.md`](./java/README.ko.md#실행)
- Kotlin: [`kotlin/README.ko.md`](./kotlin/README.ko.md#실행)

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

## 검증

두 process가 서로를 받아들이면 각 언어 README의 "검증"에 있는 `PEER_READY` 로그 줄이
찍히고, 아래 호출이 `200`과 함께 profile을 돌려준다.

```bash title="linux"
curl -sf http://127.0.0.1:5280/players/p1/profile | grep -q '"playerId":"p1"'
```

```powershell title="windows"
$profile = Invoke-RestMethod -Uri 'http://127.0.0.1:5280/players/p1/profile'
if ($profile.playerId -ne 'p1') { throw "unexpected profile: $($profile | ConvertTo-Json -Compress)" }
```

## 문제 해결

언어마다 포트·키 prefix가 달라 항목을 나눴다. Docker/Redis·JDK 관련 오류(연결 거부,
`UnsupportedClassVersionError`, `No matching toolchain found`)와 stale Redis 키 정리는
[`java/README.ko.md`](./java/README.ko.md#문제-해결)와
[`kotlin/README.ko.md`](./kotlin/README.ko.md#문제-해결)에 각각 있다.
