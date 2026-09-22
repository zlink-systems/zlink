[English](./README.md) | [한국어](./README.ko.md)

# ZLink Java/Kotlin quickstart

가장 단순한 프로젝트다. Location Store 없이 각 client가 server endpoint를 직접 지정하고, 두
process가 channel로 한 번 호출한다. 사이트의 `framework/doc/framework/java/quickstart.ko.md`와
`framework/doc/framework/kotlin/quickstart.ko.md` 페이지는 이 파일에서 코드 블록을 읽는다.
이 디렉터리는 `zlink-java-examples` 저장소의 `quickstart/`이며, 하나의 Gradle build 안에
`java/`와 `kotlin/` subproject가 있다.

| | 목적 |
|---|---|
| **quickstart** (여기) | package 설치와 첫 응답 확인. 기능을 추가하지 않는다 |
| tutorial (`tutorial/`) | 기능을 단계별로 추가한다. 기능별 guide가 이 코드를 읽는다 |
| samples (`samples/`) | 완결된 업무 흐름을 보이는 application을 제공한다 |

## 전제 조건

- JDK 25 이상. Java와 Kotlin subproject의 toolchain이 25로 고정되어 있다.
- 이 디렉터리에 포함된 Gradle wrapper. 첫 빌드에서 Gradle과 package를 Maven Central에서
  내려받는다.
- Redis나 다른 외부 service는 필요하지 않다.

## 내려받기와 설치

[`zlink-java-examples`](https://github.com/zlink-systems/zlink-java-examples) 저장소를
clone한다. 아래 명령은 저장소의 `quickstart/`에서 실행한다.

`gradle/libs.versions.toml`이 `zlink-framework-core`, `zlink-framework-spring-boot-starter`,
`zlink-framework-kotlin`과 Spring Boot·Kotlin·Jackson의 버전을 고정한다.
`systems.zlink:zlink` binding은 목록에 넣지 않고 `zlink-framework-core`의 전이 의존에 맡긴다.

Kotlin Client는 subproject build file에서 `kotlinx-coroutines-reactor`도 version 없이 선언한다.
이 package의 version은 전이 coroutine 제약으로 해석된다.

## 빌드

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

## 실행

한 번에 한 언어의 pair만 실행한다. 각 Server는 `tcp://0.0.0.0:7101`에서 듣고 `greeting`
channel을 처리한다. 각 Client는 `tcp://0.0.0.0:7102`에서 듣고
`tcp://127.0.0.1:7101`에 연결하며, `http://127.0.0.1:5080`에서 `GET /hello/{name}`을
제공한다.

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

Kotlin pair는 Java process를 종료한 뒤 같은 명령을 `kotlin/` 경로에서 실행한다
(`./kotlin/Server/build/install/Server/bin/Server`, `.../Client`).

## 검증

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

Java와 Kotlin pair 모두 endpoint가 HTTP 상태 코드 200과 `hello, world`를 반환한다.

## 문제 해결

| 증상 | 원인과 조치 |
|---|---|
| 7101, 7102, 5080이 이미 사용 중이다 | 다른 언어 pair를 실행하기 전에 실행 중인 pair를 종료한다 |
| curl 요청이 연결되지 않는다 | Server를 먼저 실행한 뒤 Client를 실행하고 process 출력을 확인한다 |
| 요청에 대상이 없다 | client의 `peerConnections().connect` endpoint와 server의 `listen` endpoint를 같게 둔다 |
| Server가 시작 직후 종료된다 | 각 비웹 Server entry point에 `setKeepAlive(true)`를 유지한다 |
| Java Client에서 path variable 오류가 발생한다 | Java Client compiler option에 `-parameters`를 유지한다 |
| Kotlin Client coroutine 요청이 실패한다 | Client 의존성에 `kotlinx-coroutines-reactor`를 유지한다 |
| Kotlin class 또는 payload가 실패한다 | Kotlin Spring plugin과 `jackson-module-kotlin`을 유지한다 |

## 구성

| 경로 | 내용 |
|---|---|
| `java/Shared`, `kotlin/Shared` | Java record와 Kotlin data class 계약 |
| `java/Server`, `kotlin/Server` | `greeting` handler를 등록하고 7101 port에서 듣는 process |
| `java/Client`, `kotlin/Client` | server에 연결하고 5080 port에서 `GET /hello/{name}`을 제공하는 process |
| `gradle/libs.versions.toml` | package와 plugin의 중앙 version 고정 |
| `gradlew`, `gradlew.bat` | Linux와 Windows용 Gradle wrapper 실행기 |

## 내 프로젝트에 옮길 것

- framework, Spring Boot, Kotlin, Jackson version을 명시하는
  `gradle/libs.versions.toml`. `systems.zlink:zlink`는 framework의 전이 의존에 맡긴다.
- `Shared`에 있는 Java record와 Kotlin data class 계약 형태.
- mesh 이름, `listen(...)`, `channelName(...).server().addRequestHandler(...)`, 비웹
  process의 `setKeepAlive(true)`를 담은 Server `ZLinkFrameworkConfigurer` 블록.
- `channelName(...).client()`, `peerConnections().connect(...)`,
  `ZLinkRouteClient` 호출부를 담은 Client 블록.
- 실제 서비스에서는 수동 peer connection 대신 Redis와 같은 Location Store를 주로 사용한다.
  이 quickstart는 해당 서비스 의존성을 사용하지 않는다.
