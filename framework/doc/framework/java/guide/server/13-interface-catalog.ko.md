---
title: "13. 주요 타입 사용 색인 · Java"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: 운영 — 메트릭 · drain · readiness](12-operations.ko.md) | [다음: 샘플 고르기](14-samples.ko.md)
<!-- framework-adapter-nav:end -->

# 13. 주요 타입 사용 색인

> **이 장의 계약 소유 문서** —
> [Java exact interface 목차](../../../common/spec/server/languages/java/interfaces/README.ko.md)가
> 정확한 signature를 소유한다. 이 챕터는 application에서 자주 쓰는 public interface를
> 기능별로 찾는 안내서다.

Java 표면은 **어떻게 얻는지**로 나눠 읽으면 빠르다. bean으로 주입받는 것, 직접 구현하는
것, 시작 단계 builder가 돌려주는 것, 호출이 돌려주는 것 넷이다.

## 1. Channel messaging

호출하는 쪽은 client를 생성자로 주입받는다.

```java
@Service
public class OrderService {
    private final ZLinkRouteClient client;

    public OrderService(ZLinkRouteClient client) {
        this.client = client;
    }

    public CompletionStage<OrderPlaced> place(PlaceOrder request) {
        return client
            .requestToChannel("orders", request)
            .timeout(Duration.ofSeconds(3))
            .submit(OrderPlaced.class);
    }
}
```

| Interface | Application에서 하는 일 |
| --- | --- |
| `ZLinkRouteClient` | ChannelName 또는 관리 대상 Node RID로 send · request |
| `ZLinkFanoutClient` | classic fanout channel에 event publish |
| `ZLinkSpotPublisherClient` | Spot 밖에서 Logical Multicast 발행 |
| `ZLinkSendCall` · `ZLinkRequestCall` · `ZLinkPublishCall` | 각 호출의 timeout · metadata · terminal |
| `ZLinkMessageContext` · `ZLinkRouteMessageContext` | 이 dispatch의 metadata와 출처 |
| `ZLinkMessage` | 아직 decode하지 않은 payload |

받는 쪽은 handler interface를 구현한다.

| Handler interface | 받는 것 |
| --- | --- |
| `ZLinkRequestHandler<TReq, TRes>` | channel request |
| `ZLinkSendHandler<TMsg>` | channel one-way send |
| `ZLinkFanoutHandler<TEvent>` | classic fanout event |
| `ZLinkRouteRequestHandler` · `ZLinkRouteSendHandler` | Node direct |

**attribute로 묶는 방법도 있다.** `@ZLinkHandlerGroup`으로 class를 묶고 메서드에
`@ZLinkRequest` · `@ZLinkSend` · `@ZLinkPublish`를 붙이면 interface 구현 없이 등록된다.
어느 channel에 노출할지는 등록이 정한다.

## 2. Topology 등록

`ZLinkFrameworkConfigurer`가 받는 `ZLinkFrameworkOptions` 아래의 builder들이다. Spring
컨텍스트 시작 뒤에는 쓸 수 없다.

| Interface | 무엇을 등록하나 |
| --- | --- |
| `ZLinkFrameworkOptions` | 루트 — codec · handler 탐색 · location store · dispatch |
| `ZLinkMeshNodeBuilder` | MeshNode 하나(`addRouteMesh`) |
| `ZLinkMeshChannelBuilder` → `...ServerBuilder` · `...ClientBuilder` | 그 node의 channel 역할 |
| `ZLinkMeshObjectRoleBuilder` → `...ServerBuilder` · `...ClientBuilder` | Object role과 Spot · Actor 등록 |
| `FanoutChannelBuilder` | classic fanout channel |
| `ClientServerChannelBuilder` | client · server 짝 channel |
| `ZLinkStreamNodeBuilder` | STREAM node |
| `ZLinkMeshPeerConnections` · `ZLinkEndpointConnections` | 수동 peer 연결 |
| `ZLinkMeshNodeSocketConfig` | 소켓 상한([16. Options](16-options.ko.md) §3.1) |
| `ZLinkUserSpotFactoryBuilder` · `ZLinkInstanceSpotFactoryBuilder` · `ZLinkActorFactoryBuilder` | 등록 시 정책 |
| `ZLinkCodecRegistryBuilder` · `ZLinkCodecExtension` | 직렬화 형식 |
| `ZLinkMetadataPolicyBuilder` | metadata 전달 정책 |

## 3. Spot

| Interface | 성격 |
| --- | --- |
| `ZLinkSpot` · `ZLinkSpot<TActor>` | User Spot — 구현한다 |
| `ZLinkEntrySpot<TActor>` | Entry Spot — 구현한다 |
| `ZLinkInstanceSpot` | Instance Spot — 구현한다 |
| `ZLinkSpotContext` · `ZLinkEntrySpotContext` · `ZLinkInstanceSpotContext` | 생성자로 받는다 |
| `ZLinkSpotManager` | 주입받아 Spot을 만들고 찾는다 |
| `ZLinkSpotCreateCall` · `ZLinkSpotGetOrCreateCall` | 생성 호출 |
| `ZLinkSpotCreateResult` · `ZLinkSpotCreateState` | 생성 결과와 세 상태 |
| `ZLinkSpotCreateResponse` | 생성 callback의 accept · reject |
| `ZLinkSpotActorJoinResult` | join admission의 accept · reject |
| `ZLinkSpotClosingContext` · `ZLinkSpotCloseReason` | 닫히는 중의 deadline과 사유 |
| `ZLinkSpotHandlerRegistry` · `ZLinkInstanceSpotHandlerRegistry` | `configure()`에서 handler 등록 |
| `ZLinkSpotOutbound` | Spot에서 나가는 호출 |
| `ZLinkSpotRelocationAdapter<TSpot>` | 상태를 담고 푸는 adapter |
| `ZLinkSpotRelocationReadyCall` · `...Completion` · `...Outcome` | 이전 가능 시점 신호와 결과 |
| `ZLinkSpotSendCall` · `ZLinkSpotRequestCall` | Spot 대상 호출 |

Spot이 받는 handler는 넷이다.

| Handler interface | 받는 것 |
| --- | --- |
| `ZLinkSpotPacketHandler<TSpot, TMsg>` | Spot 앞 one-way packet |
| `ZLinkSpotRequestHandler<TSpot, TReq, TRes>` | Spot 앞 request |
| `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` | Logical Multicast 구독 이벤트 |
| `ZLinkSpotTimerHandler<TSpot>` | timer tick |
| `ZLinkSpotActorSendHandler` · `ZLinkSpotActorRequestHandler` | member Actor 앞 packet · request |

timer 관련 타입은 `ZLinkTimer`(취소 핸들) · `ZLinkTimerOptions` ·
`ZLinkTimerOverrunPolicy` · `ZLinkTimerTick`이다.

worker 관련 타입은 `ZLinkWorkerCall<T>` · `ZLinkWorkerTask<T>`(동기) ·
`ZLinkIoWorkerTask<T>`(비동기) · `ZLinkWorkerCancellation`이고, 실패는
`ZLinkWorkerQueueFullException` · `ZLinkWorkerTimeoutException` ·
`ZLinkWorkerFailedException`으로 갈린다.

## 4. Actor

| Interface | 성격 |
| --- | --- |
| `ZLinkActor` | 구현한다 |
| `ZLinkActorContext` | 생성자로 받는다. join · bound session 접근 |
| `ZLinkActorManager` | 주입받아 Actor를 만들고 찾는다 |
| `ZLinkActorClient` | ActorId로 send · request |
| `ZLinkActorFactory` | 생성 방법 |
| `ZLinkActorCreateCall` · `ZLinkActorGetOrCreateCall` | 생성 호출 |
| `ZLinkActorCreateResult` | 생성 결과 — `Existing` · `Created` · `Rejected` |
| `ZLinkActorCreateResponse` | Entry Spot의 admission 응답 |
| `ZLinkActorJoinCall` · `ZLinkActorJoinCompletion` · `ZLinkActorJoinOperationId` | join 예약과 완료 |
| `ZLinkActorRelocationAdapter<TActor>` · `ZLinkRelocationCancellation` | 상태 이전 |
| `ZLinkActorHandlerRegistry` | Actor handler 등록 |
| `ZLinkBoundSession` · `ZLinkBoundSessionSendCall` | bound session으로 push |

**생성 결과와 join 완료는 sealed 계층이다.** `instanceof` 패턴 매칭이나 `switch`로
가른다.

## 5. STREAM session

| Interface | 성격 |
| --- | --- |
| `ZLinkSession` | 구현한다. `configure` · `onDispatch` · lifecycle callback |
| `ZLinkSessionContext` | 생성자로 받는다 |
| `ZLinkSessionDispatchContext` | 이 packet의 dispatch 정보 |
| `ZLinkSessionClient` | reply · send |
| `ZLinkSessionReplyCall` · `ZLinkSessionSendCall` | 각 호출 |
| `ZLinkSessionActor` · `ZLinkSessionActors` | session에 bind된 Actor |
| `ZLinkSessionPacketDispatcher` · `ZLinkTypedSessionPacketHandler` | typed packet 처리 |
| `ZLinkStreamError` · `ZLinkStreamSessionError` | 오류 통지 |
| `ZLinkStreamCodec` · `ZLinkStreamCompressionCodec` | 인코딩과 압축 |

## 6. Location과 relocation

| Interface | 성격 |
| --- | --- |
| `ZLinkLocationStore` · `ZLinkRelocationStore` | 직접 구현하거나 제공 구현을 쓴다 |
| `ZLinkRedisLocationStore` · `ZLinkRedisLocationOptions` | Redis 구현과 설정 |
| `ZLinkRedisRelocationStore` · `ZLinkRedisRelocationOptions` | 〃 |
| `ZLinkLocationOptions` | 동작 값([16. Options](16-options.ko.md) §5) |
| `ZLinkLocationReadiness` | 필요한 peer가 Ready인지 |
| `ZLinkLocationRuntimeQuery` | 상태와 topology 조회 |

store를 직접 구현할 일은 드물다. `ZLinkStore*` · `ZLinkBlob*` 계열은 그때만 본다.

## 7. Host와 관측

| Interface | 성격 |
| --- | --- |
| `ZLinkFrameworkRuntime` | host 상태와 relocate · shutdown |
| `ZLinkFrameworkRuntimeStatus` · `ZLinkFrameworkRuntimeState` | 상태 record와 상태값 |
| `ZLinkRouteMeshRuntime` | MeshNode snapshot과 observation |
| `ZLinkRouteMeshRuntimeOptions` | 실행 중 가중치 조정 |
| `ZLinkClientServerRuntime` · `ZLinkFanoutRuntime` | 해당 channel의 상태 |
| `ZLinkMeshNodeSnapshot` · `ZLinkMeshPeerSnapshot` · `ZLinkMeshChannelSnapshot` | snapshot record |
| `ZLinkDispatchOptions` · `ZLinkDiagnosticsOptions` | 진단 수준 |
| `ZLinkMessageFlowObserver` · `ZLinkMessageFlowEvent` | 메시지 흐름 기록 |
| `ZLinkMetricsCustomizer` | Micrometer registry 조정 |

## 8. 실패 타입

| Exception | 언제 |
| --- | --- |
| `ZLinkConfigurationException` | 등록이 잘못됐다. 컨텍스트 시작에서 난다 |
| `ZLinkFrameworkException` | 런타임 실패. `kind()` · `retriable()`로 가른다 |
| `ZLinkRequestFailureException` | request가 실패로 끝났다. `ZLinkRequestFailureReason`을 담는다 |
| `ZLinkOperationCanceledException` | 취소됐다 |
| `ZLinkWorkerQueueFullException` 외 | worker 실패 세 갈래 |

`ZLinkFrameworkErrorKind`가 실패 갈래를 담는 enum이다.

## 9. 어디서 오는가

| 얻는 방법 | 해당 interface |
| --- | --- |
| 구현한다 | `ZLinkSpot` · `ZLinkEntrySpot` · `ZLinkInstanceSpot` · `ZLinkActor` · `ZLinkSession` · `*Handler` 계열 |
| 생성자로 받는다(context) | `ZLinkSpotContext` 계열 · `ZLinkActorContext` · `ZLinkSessionContext` |
| bean으로 주입받는다 | `ZLinkRouteClient` · `ZLinkFanoutClient` · `ZLinkActorClient` · `ZLinkSpotManager` · `ZLinkActorManager` · `ZLinkFrameworkRuntime` · `ZLinkRouteMeshRuntime` · `ZLinkLocationRuntimeQuery` |
| 시작 단계 builder가 돌려준다 | `ZLinkMeshNodeBuilder` 계열 · `ZLinkStreamNodeBuilder` · `FanoutChannelBuilder` |
| 호출이 돌려준다 | `*Call` · `*Result` · `*Response` · `*Snapshot` |

**handler와 Spot·Actor·Session은 bean이 아니다.** framework가 만들고 생성자 인자만
Spring 컨테이너에서 주입된다([2. 시작하기](02-getting-started.ko.md) §3).

## 10. 관련 문서

- 정확한 signature: [Java exact interface 목차](../../../common/spec/server/languages/java/interfaces/README.ko.md)
- 등록 진입점: [2. 시작하기](02-getting-started.ko.md)
- 옵션과 기본값: [16. Options](16-options.ko.md)
- 관측 표면: [11. Monitoring](11-monitoring.ko.md)
