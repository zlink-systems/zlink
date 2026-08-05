# ZLink Framework for Kotlin -- 문서

> 이 묶음은 `Kotlin`(Spring Boot) 사용자를 위한 ZLink Framework 문서다.
> `zlink-framework-kotlin`은 Java `zlink-framework` 런타임을 그대로 재사용하는
> 얇은 coroutine idiom 레이어다. Java 표면은
> [Java spec](../common/spec/server/languages/java/README.ko.md)을 따르고, Kotlin 전용
> 공개 계약은 [Kotlin spec](../common/spec/server/languages/kotlin/README.ko.md)에 고정한다.
> 내부 기준은 [Java/Kotlin 문서](../java/README.ko.md)를 공유한다. Kotlin 사용 guide는 11.0 public
> interface와 sample이 확정된 뒤 Kotlin 전용으로 다시 작성한다. 공통 의미는
> [공통 스펙](../common/README.ko.md)을 따른다.

비동기 실행, `CompletionStage`, Kotlin coroutine wrapper의 공통 의미는
[비동기 실행과 coroutine 정책](../common/spec/05-async-execution-policy.ko.md)을 따른다.

Sample과 E2E의 설정 파일, 환경 변수 금지와 `@ConfigurationProperties` binding 기준은
[Sample/E2E 설정 정책](../common/sample-e2e-configuration-policy.ko.md)을 따른다.

서버 framework와 별도로 사용하는 client library의 coroutine 사용법은
[HTTP client 가이드](guide/http-client/README.ko.md)와
[Stream connector 가이드](guide/stream-connector/README.ko.md)에서 확인한다.

## 0. Kotlin 표면 한눈에

`zlink-framework-kotlin`은 새 transport를 만들지 않는다. Java framework가 노출하는
같은 channel·Spot·actor·stream 위에 coroutine 표면만 얹는다.

| Java 표면 | Kotlin 표면 |
|-----------|-------------|
| `ZLinkRequestHandler<T, R>` (plain `TReply` 반환) | `ZLinkSuspendingRequestHandler<T, R>` (`suspend fun handle`) |
| `ZLinkSendHandler` / `ZLinkFanoutHandler` | `ZLinkSuspendingSendHandler` / `ZLinkSuspendingPublishHandler` |
| `ZLinkSpot<TActor>` / `ZLinkEntrySpot<TActor>` | `ZLinkSuspendingSpot<TActor>` / `ZLinkSuspendingEntrySpot<TActor>` (actor admission, joined, leave를 `suspend`로 처리) |
| Java relocation policy와 opaque byte adapter | `ZLinkRelocationPolicy.snapshot(Adapter::class.java)` |
| `ZLinkSession` | `ZLinkSuspendingSession` (`onConnectedSuspending` 등) |
| `client.requestToChannel(...).submit(R::class.java)` | `client.request<R>(channel, msg)` / `call.awaitReply<R>()` |
| `connector.on(name) { ... }` callback | `connector.kotlin().messages(name): Flow<...>` |

Coroutine handler configuration의 정확한 signature는 Kotlin interfaces가 소유한다.

## 2. 공개 계약 spec

Kotlin은 같은 Spring Boot runtime 위에 coroutine 확장을 더한다. 그대로 사용하는
Java 타입은 Java spec을 따르고, Kotlin에서 새로 노출하는 `suspend`, `Flow`, adapter
시그니처는 Kotlin spec을 따른다.

| 문서 | 범위 |
|------|------|
| [Kotlin spec 목차](../common/spec/server/languages/kotlin/README.ko.md) | Kotlin 전용 공개 계약 문서 목록 |
| [Kotlin interfaces](../common/spec/server/languages/kotlin/interfaces/README.ko.md) | coroutine·DSL exact public signature |
| [Java spec 목차](../common/spec/server/languages/java/README.ko.md) | Kotlin이 그대로 사용하는 Java 공개 계약 |
| [Java interfaces](../common/spec/server/languages/java/interfaces/README.ko.md) | Kotlin이 재사용하는 Java 정본 type과 builder |
| [Channel messaging](../common/spec/server/languages/java/interfaces/channel-messaging.ko.md) | channel 등록, outbound client와 dispatch |
| [Spot](../common/spec/server/languages/java/interfaces/spots.ko.md) | Spot lifecycle와 factory |
| [Actor](../common/spec/server/languages/java/interfaces/actors.ko.md) | actor factory, relocation adapter와 bound session |
| [STREAM](../common/spec/server/languages/java/interfaces/stream-session.ko.md) | stream node와 header session |
| [stream-connector](../common/spec/stream-connector/languages/java/03-stream-connector.ko.md) | Java/Kotlin Stream Connector |
| [Location과 maintenance](../common/spec/server/languages/java/interfaces/location-maintenance.ko.md) | discovery, authority와 relocation |
| [Monitoring](../common/spec/server/languages/java/interfaces/monitoring.ko.md) | runtime event와 typed handler |

## 3. 내부 기준 — Java/Kotlin 공유

구현 구조, lifecycle, regression 기준은 같은 런타임을 쓰므로 **Java/Kotlin
`internals/`를 공유**한다.

| 문서 | 범위 |
|------|------|
| [backend-dependency-policy](../java/internals/backend-dependency-policy.ko.md) | Java binding 의존 격리 |
| [공통 내부 구조](../common/internals/README.ko.md) | 네 언어가 공유하는 runtime 아키텍처 결정 |
| [regression-test-matrix](../java/internals/regression-test-matrix.ko.md) | JVM contract, E2E와 performance smoke 기준 |

## 4. 샘플 (Kotlin)

샘플은 Java와 같은 scenario set을 Kotlin coroutine 구현으로 제공한다. 정본 6종은
per-app 문서로, 기능 축 샘플은 별도 문서로 둔다.

정본 6종의 서버 역할, 메시지 계약, 상태 전이와 완료 기준은
[공통 샘플](../common/sample/README.ko.md)이 소유한다. Kotlin 문서는 이 계약을 다시
서술하지 않는다.

| 문서 | 범위 |
|------|------|
| [samples README](../../../languages/java/samples/README.md) | Java/Kotlin sample 구조와 실행 방법 |
