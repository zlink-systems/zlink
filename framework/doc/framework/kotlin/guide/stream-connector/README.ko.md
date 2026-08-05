# Kotlin Stream Connector 사용 안내

Kotlin Stream Connector는 Java connector를 coroutine과 `Flow`로 사용할 수 있게 감싼다.
transport, reconnect, codec과 queue는 Java connector가 그대로 담당하며 Kotlin wrapper가 별도
network connection을 만들지는 않는다.

정확한 public contract는
[Java/Kotlin Stream Connector spec](../../../common/spec/stream-connector/languages/java/03-stream-connector.ko.md)이
소유한다. Java `CompletionStage` 표면을 직접 사용할 때는
[Java Stream Connector 사용 안내](../../../java/guide/stream-connector/README.ko.md)를 본다.

## 1. 의존성 추가

사용 중인 ZLink 배포 버전을 `<version>`에 지정한다. Kotlin 모듈이 Java connector를 public
dependency로 제공하므로 application에서 connector 모듈을 다시 선언하지 않는다.

```kotlin
dependencies {
    // Java connector와 coroutine await, typed wait, Flow wrapper를 함께 제공한다.
    implementation("systems.zlink:zlink-framework-kotlin:<version>")
}
```

## 2. connector 만들기

Java connector를 만든 뒤 `kotlin()`으로 감싼다. 기본 options는 `MANUAL` dispatch, heartbeat,
자동 reconnect와 JSON typed codec을 사용한다.

```kotlin
import java.net.URI
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions

val connector = ZLinkStreamConnectorFactory
    .create(
        ZLinkStreamConnectorOptions.createDefault(
            URI.create("tcp://127.0.0.1:19000"),
        ),
    )
    .kotlin()

// connect가 완료되거나 실패할 때까지 coroutine을 suspend한다.
connector.connect().await()
```

application을 종료할 때는 `connector.close().await()`로 종료 완료를 기다린다. 종료된 connector는
다시 연결할 수 없다.

## 3. Coroutine으로 송신하고 reply 받기

typed payload는 기본 JSON codec을 사용한다. packet name은 annotation 또는 type 이름으로 정해지며,
정확한 규칙과 호출별 override는 공통 contract를 따른다.

```kotlin
import systems.zlink.framework.kotlin.awaitReply

data class LoginRequest(val userId: String)
data class LoginReply(val sessionId: String)
data class PresenceChanged(val userId: String, val online: Boolean)

// one-way send가 끝나거나 실패할 때까지 기다린다.
connector
    .send(PresenceChanged("user-1", true))
    .await()

// reply type은 reified type argument로 한 번만 지정한다.
val reply: LoginReply = connector
    .request(LoginRequest("user-1"))
    .awaitReply<LoginReply>()
```

기본 `MANUAL` mode에서 request completion과 등록된 callback을 실행하려면 application이 선택한
coroutine 또는 event loop에서 `connector.dispatch().await()`를 반복 호출해야 한다. request를
기다리는 coroutine과 dispatch를 수행하는 coroutine을 같은 순차 작업으로 묶으면 reply completion을
처리할 수 없으므로 분리한다.

```kotlin
suspend fun onApplicationTick() {
    // 기존 UI loop, game loop 또는 scheduler의 tick에서 호출한다.
    connector.dispatch().await()
}
```

`IMMEDIATE` mode는 별도의 dispatch 호출 없이 receive 경로에서 callback을 실행한다. handler가
오래 실행되면 receive와 backpressure 전달이 함께 지연되므로 짧은 callback에만 사용한다.

## 4. Server push 기다리기

특정 typed push 하나를 기다릴 때는 `waitFor<T>()`를 사용한다. `where`는 message 전체를 받아
payload와 metadata를 함께 검사한다.

```kotlin
import java.time.Duration

data class MaintenanceNotice(
    val region: String,
    val startsAt: String,
)

val notice = connector
    .waitFor<MaintenanceNotice>()
    .timeout(Duration.ofSeconds(5))
    .where { message -> message.payload.region == "ap-northeast" }
    .await()
```

`waitFor`는 아직 소비하지 않은 수신 message를 직접 가져오므로 `MANUAL` mode에서도 별도 dispatch가
필요하지 않다. 같은 packet을 handler와 `waitFor`가 동시에 소비하도록 설계하지 않는다.

## 5. Flow로 연속 message 받기

packet name으로 들어오는 raw message stream은 `messages(...)`로 받고, connector error는
`errors()`로 받을 수 있다. collector를 취소하면 내부 handler 등록도 해제된다.

```kotlin
import kotlinx.coroutines.flow.collect

connector.messages("MaintenanceNotice").collect { message ->
    // raw payload가 필요할 때 사용한다. 일반 업무 DTO는 typed wait/request를 우선한다.
    println("packet=${message.packetName} flow=${message.flowId}")
}
```

## 6. 운영 시 확인할 항목

- `MANUAL` mode에서는 dispatch coroutine이나 application tick이 중단되지 않는지 확인한다.
- payload 크기 제한과 수신 queue 상한은 Java connector options에서 설정한다.
- 운영 TLS/WSS에서는 서버 인증서와 hostname 검증을 유지한다.
- reconnect와 heartbeat 값을 바꿀 때는 장애 감지 시간과 재연결 부하를 함께 검토한다.
- application scope가 끝날 때 Flow collection을 취소하고 connector 종료를 기다린다.
