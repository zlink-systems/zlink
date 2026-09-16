# Kotlin Quickstart — 빈 프로젝트에서 첫 요청까지

> **이 장의 계약 소유 문서** — 없다. API의 정식 계약은
> [Kotlin 스펙](../common/spec/server/languages/kotlin/README.ko.md)이 다룬다.

저장소의 [`framework/languages/java/quickstart/kotlin/`](../../../languages/java/quickstart/kotlin/)
프로젝트다. 아래 코드 블록은 사이트를 빌드할 때 그 파일에서 읽는다. Java판과 Kotlin판이 한
Gradle 빌드 안에 있다.

location store 없이 process 둘이 서로의 endpoint를 직접 지정해 request/reply 한 번을
주고받는다. 다음 단계는 [설치와 첫 동작](guide/server/02-getting-started.ko.md)이다.

## 전제

- JDK 25 이상. 게시된 `zlink-framework-core` 0.12.0의 class file major는 69(Java 25)다
- Maven Central 접근

## 1. 패키지 버전

`systems.zlink:zlink`(binding)는 적지 않는다. `zlink-framework-core`가 의존 버전을 선언한다.

```toml title="gradle/libs.versions.toml"
--8<-- "framework/languages/java/quickstart/gradle/libs.versions.toml"
```

## 2. 공유 계약

```kotlin title="Shared"
--8<-- "framework/languages/java/quickstart/kotlin/Shared/src/main/kotlin/systems/zlink/quickstart/shared/Contracts.kt"
```

## 3. 처리하는 쪽

`addHandlersFromPackageOf`는 handler 타입을 찾기만 한다. 어느 channel에 노출할지는
`channelName(...).server()`에 따로 등록한다.

HTTP를 열지 않는 process라 `SpringApplication.setKeepAlive(true)`가 필요하다. 없으면
context refresh 직후 JVM이 종료된다.

```kotlin title="Server"
--8<-- "framework/languages/java/quickstart/kotlin/Server/src/main/kotlin/systems/zlink/quickstart/server/ServerApplication.kt"
```

## 4. 호출하는 쪽

```kotlin title="Client"
--8<-- "framework/languages/java/quickstart/kotlin/Client/src/main/kotlin/systems/zlink/quickstart/client/ClientApplication.kt"
```

## 5. 실행

```bash
cd framework/languages/java/quickstart
./gradlew :kotlin:Server:installDist :kotlin:Client:installDist

# 터미널 두 개. server를 먼저 실행한다.
./kotlin/Server/build/install/Server/bin/Server
./kotlin/Client/build/install/Client/bin/Client

curl http://127.0.0.1:5080/hello/world
```

응답은 `hello, world`, 상태 코드 200이다.

## 옮겨 갈 것

| 파일 | 내용 |
|---|---|
| `gradle/libs.versions.toml` | `zlinkFramework` 한 항목이 세 artifact를 고정한다. binding은 전이 의존으로 둔다 |
| Shared | 두 process가 공유하는 메시지 타입 |
| Server | `addRouteMesh` → `listen` → `channelName(...).server().addRequestHandler(...)`, 그리고 `setKeepAlive(true)` |
| Client | `channelName(...).client()`, `peerConnections().connect(...)`, `requestToChannel(...)` |

수동 `peerConnections().connect` 대신 location store를 쓰는 구성은
[10. Location](guide/server/10-location.ko.md)이 다룬다. 빌드 설정에서 더 필요한 항목은
프로젝트의 `README.md`가 정리한다.
