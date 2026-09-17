# Java Quickstart — 설치부터 첫 요청까지

!!! info "이 장을 읽고 나면"

    패키지를 설치하고, 두 process가 서로 호출하는 최소 project를 실행할 수 있다.

저장소의 [`framework/languages/java/quickstart/java/`](../../../languages/java/quickstart/java/)
프로젝트다. 아래 코드 블록은 사이트를 빌드할 때 그 파일에서 읽는다. Java판과 Kotlin판이 한
Gradle 빌드 안에 있다. location store 없이 process 둘이 서로의 endpoint를 직접 지정해
request/reply 한 번을 주고받는다.

## 1. 설치

- JDK 25 이상. 게시된 `zlink-framework-core` 0.16.0의 class file major는 69(Java 25)다
- Maven Central 접근

Maven Central에서 받는다. 서버 하나를 만들 때 필요한 최소 조합은 다음과 같다.

```kotlin
// 계약과 runtime
implementation("systems.zlink:zlink-framework-core")
// DI·수명주기 등록
implementation("systems.zlink:zlink-framework-spring-boot-starter")
```

**`systems.zlink:zlink`(binding)는 적지 않는다.** `zlink-framework-core`가 의존 버전을
선언한다.

```toml title="gradle/libs.versions.toml"
--8<-- "framework/languages/java/quickstart/gradle/libs.versions.toml"
```

필요할 때 추가하는 artifact는 다음과 같다.

| artifact | 언제 추가하나 |
| --- | --- |
| `zlink-framework-locations-redis` | Redis location store로 자동 연결을 사용할 때([Location](guide/server/25-location.ko.md)) |
| `zlink-framework-codec-protobuf` · `-codec-msgpack` | 기본 JSON codec 대신 사용할 때([Handler와 메시지 처리](guide/server/31-handler-dispatch.ko.md#3-codec--payload를-바이트로-바꾼다)) |
| `zlink-stream-connector` | 외부 client(게임 client·모바일)를 만들 때([STREAM](guide/server/23-stream.ko.md)) |
| `zlink-http-client` | 서버에서 HTTP를 호출할 때([HTTP Client 가이드](guide/http-client/README.ko.md)) |

라이선스는 계층마다 다르다 — core·binding은 MPL-2.0, framework는 FSL-1.1-ALv2,
`zlink-http-client`는 Apache-2.0이다. 서비스를 만들어 파는 데 드는 비용은 없다
([ZLink의 적용 범위](guide/server/17-alternative.ko.md#8-라이선스--사용하는-데-드는-비용)).

## 2. 공유 계약

```java title="Shared"
--8<-- "framework/languages/java/quickstart/java/Shared/src/main/java/systems/zlink/quickstart/shared/Hello.java"
```

## 3. 처리하는 쪽

`addHandlersFromPackageOf`는 handler 타입을 찾기만 한다. 어느 channel에 노출할지는
`channelName(...).server()`에 따로 등록한다.

HTTP를 열지 않는 process라 `SpringApplication.setKeepAlive(true)`가 필요하다. 없으면
context refresh 직후 JVM이 종료된다.

```java title="Server"
--8<-- "framework/languages/java/quickstart/java/Server/src/main/java/systems/zlink/quickstart/server/ServerApplication.java"
```

## 4. 호출하는 쪽

```java title="Client"
--8<-- "framework/languages/java/quickstart/java/Client/src/main/java/systems/zlink/quickstart/client/ClientApplication.java"
```

## 5. 실행

```bash
cd framework/languages/java/quickstart
./gradlew :java:Server:installDist :java:Client:installDist

# 터미널 두 개. server를 먼저 실행한다.
./java/Server/build/install/Server/bin/Server
./java/Client/build/install/Client/bin/Client

curl http://127.0.0.1:5080/hello/world
```

응답은 `hello, world`, 상태 코드 200이다.

## 6. 첫 실행이 안 될 때 확인할 항목

| 증상 | 확인할 항목 |
| --- | --- |
| artifact를 찾지 못한다 | §1의 artifact 이름을 그대로 적었는지 확인한다. binding 버전은 따로 고정하지 않는다 |
| startup이 실패한다 | 두 process의 mesh 이름이 같은지, listen endpoint가 다른 process와 겹치지 않는지 확인한다 |
| server가 바로 종료된다 | `SpringApplication.setKeepAlive(true)`를 호출했는지 확인한다 |
| 호출이 대상 없음으로 끝난다 | 받는 쪽이 그 channel 이름을 server 역할로 등록했는지, 두 process가 peer로 연결됐는지 확인한다 |
| 응답이 오지 않는다 | 보낸 쪽이 `request`를 썼는지 확인한다. `send`는 응답을 받지 않는다 |

## 7. 옮겨 갈 것

| 파일 | 내용 |
|---|---|
| `gradle/libs.versions.toml` | `zlinkFramework` 한 항목이 세 artifact를 고정한다. binding은 전이 의존으로 둔다 |
| Shared | 두 process가 공유하는 메시지 타입 |
| Server | `addRouteMesh` → `listen` → `channelName(...).server().addRequestHandler(...)`, 그리고 `setKeepAlive(true)` |
| Client | `channelName(...).client()`, `peerConnections().connect(...)`, `requestToChannel(...)` |

빌드 설정에서 더 필요한 항목은 프로젝트의 `README.md`가 정리한다.

## 8. 다음으로 읽을 것

이 두 process는 endpoint를 서로 직접 적어 연결한다. 서버를 늘리거나 다른 주소로 다시 시작해도
호출 코드를 그대로 두려면 자동 연결이 필요하고, 그것은
[Location](guide/server/25-location.ko.md)이 다룬다.

- 개념을 먼저 확인할 때 — [핵심 개념](guide/server/03-concepts.ko.md)
- 이름으로 호출하는 경로 — [Channel 메시징](guide/server/20-channel-messaging.ko.md)
- id로 호출하는 상태 객체 — [Spot](guide/server/21-spot.ko.md) · [Actor](guide/server/22-actor.ko.md)
- 완결된 업무 흐름을 볼 때 — [샘플 고르기](guide/server/14-samples.ko.md)
