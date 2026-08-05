---
title: "13. 주요 타입 사용 색인 · Kotlin"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: 운영 — 메트릭 · drain · readiness](12-operations.ko.md) | [다음: 샘플 고르기](14-samples.ko.md)
<!-- framework-adapter-nav:end -->

# 13. 주요 타입 사용 색인

> **이 장의 계약 소유 문서** — 대부분의 표면은
> [Java 13. 주요 interface 사용 색인](../../../java/guide/server/13-interface-catalog.ko.md)이
> 소유한다. 이 장은 Kotlin이 **추가로 제공하는 것**만 모은다.

Java 장을 먼저 본다. 여기 없는 이름은 전부 Java와 같다.

## 1. suspend handler 계약

Java handler는 `CompletionStage`를 돌려주고, Kotlin 짝은 `suspend`다. **둘 중 아무거나
구현해도 되고 한 프로젝트에 섞어도 된다** — 등록이 어느 계약인지 보고 맞게 부른다.

| 받는 것 | Java | Kotlin |
| --- | --- | --- |
| channel request | `ZLinkRequestHandler` | `ZLinkSuspendingRequestHandler` |
| channel one-way send | `ZLinkSendHandler` | `ZLinkSuspendingSendHandler` |
| classic fanout event | `ZLinkFanoutHandler` | `ZLinkSuspendingPublishHandler` |
| Node direct request · send | `ZLinkRouteRequestHandler` · `ZLinkRouteSendHandler` | `ZLinkSuspendingRouteRequestHandler` · `ZLinkSuspendingRouteSendHandler` |
| Spot 앞 packet | `ZLinkSpotPacketHandler` | `ZLinkSuspendingSpotPacketHandler` |
| Spot 앞 request | `ZLinkSpotRequestHandler` | `ZLinkSuspendingSpotRequestHandler` |
| 구독 이벤트 | `ZLinkSpotSubscriptionHandler` | `ZLinkSuspendingSpotSubscriptionHandler` |
| timer tick | `ZLinkSpotTimerHandler` | `ZLinkSuspendingSpotTimerHandler` |
| member Actor 앞 packet · request | `ZLinkSpotActorSendHandler` · `...RequestHandler` | `ZLinkSuspendingSpotActorSendHandler` · `...RequestHandler` |
| Entry Spot의 Actor packet · request | `ZLinkEntrySpotActorSendHandler` · `...RequestHandler` | `ZLinkSuspendingEntrySpotActorSendHandler` · `...RequestHandler` |
| session typed packet | `ZLinkTypedSessionPacketHandler` | `ZLinkSuspendingTypedSessionPacketHandler` |

## 2. `.kotlin()` wrapper

Java client에 `.kotlin()`을 부르면 같은 호출이 suspend 표면으로 바뀐다.

| Java | `.kotlin()`이 주는 것 |
| --- | --- |
| `ZLinkClient` | `ZLinkKotlinClient` |
| `ZLinkRouteClient` | `ZLinkKotlinRouteClient` |
| `ZLinkFanoutClient` | `ZLinkKotlinFanoutClient` |
| `ZLinkActorClient` | `ZLinkKotlinActorClient` |
| `ZLinkActorManager` | `ZLinkKotlinActorManager` |

wrapper가 돌려주는 호출 타입도 Kotlin 짝이다 — `ZLinkKotlinRequestCall` ·
`ZLinkKotlinMessageSendCall` · `ZLinkKotlinActorCreateCall` ·
`ZLinkKotlinLifecycleCall` · `ZLinkKotlinBoundSession`이다.

**wrapper는 선택이다.** Java 표면을 그대로 쓰고 §3의 `await()`로 받아도 된다.

## 3. 확장 함수

wrapper가 없는 자리는 확장 함수가 메운다.

| 확장 | 무엇을 바꾸나 |
| --- | --- |
| `CompletionStage<T>.await()` | 결과를 suspend로 받는다. **turn을 안다**(§4) |
| `Flow.Publisher<T>.asFlow()` | 상태 stream을 코루틴 `Flow`로 |
| `ZLinkLocationRuntimeQuery.topology(filter, pageSize)` | 페이지 반복을 `Flow`로 |
| `ZLinkSpotHandlerRegistry.addHandler<T>()` | reified 타입으로 등록 |
| `ZLinkFrameworkOptions.routeMesh(name) { ... }` | MeshNode 등록을 블록으로 |
| `ZLinkMeshNodeBuilder.channelName(name) { ... }` | channel 등록을 블록으로 |
| `ZLinkMeshPeerConnections.connect(...)` | 여러 endpoint를 한 번에 |
| `ZLinkFrameworkOptions.configureDispatch { ... }` | 진단 설정을 블록으로 |
| `ZLinkDispatchOptions.onMessageFlow { ... }` | observer를 람다로 |
| `ZLinkMessage.decode<T>()` · `messageOf(...)` | reified decode와 생성 |
| `ZLinkStreamConnector.kotlin()` · `.messages()` · `.errors()` | connector를 suspend · `Flow`로 |
| `ZLinkStreamConnectorOptions.withLz4StreamCompression()` 외 | 압축 설정 |

## 4. `await()`를 아무 것으로나 바꾸지 않는다

`zlink-framework-kotlin`의 `CompletionStage<T>.await()`는 **framework turn을 안다.**
Spot이나 Actor의 turn 안에서 불러도 그 turn의 직렬 실행 보장을 깨지 않는다.

`kotlinx.coroutines.future.await`도 같은 이름이라 import 하나 차이로 바뀐다. turn 안에서
쓰는 코드라면 어느 쪽을 import했는지 확인한다.

```kotlin
import systems.zlink.framework.kotlin.await   // turn을 아는 쪽
```

## 5. 관련 문서

- 기능별 interface 색인: [Java 13. 주요 interface 사용 색인](../../../java/guide/server/13-interface-catalog.ko.md)
- Kotlin 레이어 개요: [1. 개요](01-overview.ko.md) §2
- Kotlin 전용 계약: [Kotlin 공개 계약](../../../common/spec/server/languages/kotlin/README.ko.md)
- 공유하는 계약: [Java exact interface 목차](../../../common/spec/server/languages/java/interfaces/README.ko.md)
