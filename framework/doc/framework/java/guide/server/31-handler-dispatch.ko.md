---
title: "Handler와 메시지 처리 · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/31-handler-dispatch.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Handler와 메시지 처리

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Channel 동작 원리](30-channel-patterns.ko.md) | [다음: STREAM의 동작 원리](38-stream-boundary.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/31-handler-dispatch.ko.md) · [C#/.NET](../../../dotnet/guide/server/31-handler-dispatch.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/31-handler-dispatch.ko.md) · [Node/TypeScript](../../../node/guide/server/31-handler-dispatch.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    여러 handler에 공통으로 적용되는 것을 다룰 수 있다 — packet 이름, filter, codec.
    이 장의 코드는 저장소의 튜토리얼과 샘플에서 가져왔다.

[Channel 메시징](20-channel-messaging.ko.md)이 handler 하나를 만들어 호출까지 가는 길을
다뤘다면, 이 장은 **여러 handler에 공통으로 적용되는 것**을 다룬다. 등록 방법의 변형, 공통
처리를 모으는 filter, 그리고 payload를 바이트로 바꾸는 codec이다.

## 1. handler 등록의 변형

### 1.1 한 MeshNode에 channel 여러 개

같은 MeshNode 위에 channel을 여러 개 등록할 수 있고, **channel마다 role이 다를 수 있다.**
받기만 하는 channel은 `Server()`로, 호출만 하는 channel은 `Client()`로 등록한다.

```java
--8<-- "framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/Program.java:doc-multi-channel-register"
```

이 node는 `zone` channel을 `Server()`로 등록해 처리하고, `report` channel은 `Client()`로
등록해 보내기만 한다. 두 등록이 같은 MeshNode 위에 나란히 있고 socket은 하나뿐이다.

### 1.2 packet 이름이 정해지는 순서

handler와 호출은 **packet 이름**으로 짝을 찾는다. 이름은 다음 순서로 정해진다.

1. handler를 등록할 때 넘긴 `packetName` 인자
2. payload 타입에 붙인 packet 이름 표시
3. 둘 다 없으면 타입 이름

packet 이름은 **등록 시 한 번** 확정되고 호출마다 다시 지정하는 표면은 없다.

**보내는 쪽과 받는 쪽이 같은 이름을 써야 한다.** 등록에만 `packetName`을 주면 보내는 쪽은
2나 3으로 타입 이름을 쓰게 되어 두 이름이 어긋나고, 그 호출은 handler를 찾지 못한 것으로
끝난다. 이름을 따로 정할 이유가 없으면 양쪽 다 생략해 3을 따른다.

서로 다른 MeshName이나 ChannelName이면 같은 packet 이름을 다시 사용해도 된다.

**이름을 직접 정하는 자리는 언어마다 다르다.** 등록 호출에 이름을 넘기는 언어가 있고, payload
타입에 이름을 표시하는 언어가 있다. 앞의 순서에서 각각 첫째와 둘째에 해당한다.

```java
--8<-- "framework/languages/java/samples/java/DeliveryDispatch/Shared/src/main/java/systems/zlink/samples/deliverydispatch/shared/contracts/Messages.java:doc-explicit-packet-name"
```

어느 쪽도 하지 않으면 타입 이름이 그대로 packet 이름이 된다. 튜토리얼의 등록은 모두 이
방식이다.

### 1.3 handler가 없는 packet의 처리 결과

| 호출 | 결과 |
| --- | --- |
| `request` | error reply로 실패한다. 호출한 쪽은 예외로 받는다 |
| `send` | 조용히 drop된다 |

drop은 호출한 쪽에 reply가 없다는 뜻이지 관측 흔적이 없다는 뜻이 아니다. 구성한
logger·telemetry provider에는 dispatch 실패가 `no_handler`·`reply_error`·`drop` structured
record로 남는다([모니터링](26-monitoring.ko.md)).

## 2. Filter — 공통 처리를 한곳에 모은다

웹 framework의 HTTP middleware는 HTTP 파이프라인 전용이라 handler에는 적용되지 않는다.
로그·검증·권한 확인·측정처럼 여러 handler에 같은 코드가 반복될 일은 filter로 모은다.

<iframe class="zlink-diagram" src="/common/diagrams/31-filter-scope.html" title="filter는 node가 받는 message를 감싼다" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/31-filter-scope.html" target="_blank">↗ 크게 보기</a></p>

filter는 **node가 받는 message**만 감싼다. 그래서, 같은 코드를
Spot이나 Actor의 handler에도 적용하려면 그쪽에 따로 둔다. 아래 절이 작성·등록 순서·범위를
차례로 다룬다.

### 2.1 filter 작성

filter interface를 구현하고 `next`를 호출한다. `next`를 호출하지 않으면 handler가
실행되지 않는다. 정확한 이름은 아래 탭에서 그 언어의 것을 본다.

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/dispatch/CallLogFilter.java:filter-implementation"
```

### 2.2 등록 순서가 실행 순서다

```java
--8<-- "framework/languages/java/tutorial/java/Server/src/main/java/systems/zlink/tutorial/server/ServerApplication.java:filter-register"
```

등록한 순서대로 handler 앞을 지나고, `next`가 끝나면 반대 순서로 빠져나온다.

```text
첫 번째 filter 앞부분
  -> 두 번째 filter 앞부분
       -> handler
     두 번째 filter 뒷부분
첫 번째 filter 뒷부분
```

각 filter는 `next`를 **최대 한 번** 호출한다. 두 번 호출하면 handler를 다시 실행하지 않고
오류로 거부한다 — 코드 실수로 분류한다.

### 2.3 filter의 적용 범위

filter는 **node가 받는 message**에 적용된다. Spot이나 Actor처럼 수명을 가진 객체가 소유한
handler에는 적용되지 않는다.

| dispatch | filter |
| --- | --- |
| channel send·request (RouteMesh와 ClientServer 모두) | 실행된다 |
| Fanout 구독 handler | 실행된다 |
| Node direct handler | 실행된다 |
| Spot handler · Actor handler | 실행되지 않는다 |
| Spot이 등록하는 Logical Multicast 구독 | 실행되지 않는다 |
| STREAM session handler | 실행되지 않는다 |

경로별로 다르게 처리하려면 `context.DispatchKind`를 본다. `ChannelSend`·`ChannelRequest`는
RouteMesh와 ClientServer를 함께 가리키므로, 둘을 구분해야 하면 `context.MeshName`을 함께
본다 — RouteMesh와 Node direct는 MeshName을 제공하고 ClientServer와 Fanout은 제공하지 않는다.

### 2.4 next를 호출하지 않았을 때의 결과

| dispatch | 호출한 쪽이 보는 결과 |
| --- | --- |
| `send` | 그 dispatch만 끝난다. 보낸 쪽은 이미 전송 접수 결과를 받았으므로 달라지는 것이 없다 |
| `request` | `Rejected` 오류 reply를 받는다. 값이 없다고 `null`이 정상 응답으로 가지 않는다 |
| Fanout 구독 | 그 handler 하나만 끝나고 같은 이벤트를 받은 다른 구독 handler는 그대로 실행된다. 발행자에게는 아무것도 전달되지 않는다 |

filter가 응답 값을 직접 만들어 돌려주는 방법은 없다. 요청을 막으려면 `next`를 호출하지 않고,
응답 내용을 바꾸려면 handler에서 처리한다.

### 2.5 인스턴스와 의존성

handler 하나를 실행하는 dispatch마다 scope가 새로 열린다. filter와 handler는 그 scope에서
각각 한 번 만들어지고 **같은 `Scoped` 서비스 인스턴스를 공유한다.** filter가 넣어 둔 값을
handler가 그대로 보는 구조이므로, 요청 단위 상태를 scoped 서비스에 담아 넘길 수 있다. filter
type을 DI에 어떤 lifetime으로 등록하든 이 규칙은 바뀌지 않는다.

Fanout은 이벤트 하나가 아니라 **일치한 구독 handler마다** dispatch가 생긴다. 따라서 filter도
그 수만큼 실행되고, 무거운 filter는 구독자가 늘수록 비용이 그만큼 커진다.

## 3. Codec — payload를 바이트로 바꾼다

### 3.1 등록

codec은 framework 등록에서 활성화한다. 등록하지 않으면 기본 codec을 사용한다.

```java
--8<-- "framework/languages/java/samples/java/Bingo/Server/Api/src/main/java/systems/zlink/samples/bingo/server/api/ApiServerApplication.java:doc-codec-register"
```

payload는 codec이 직렬화할 수 있는 DTO여야 한다. root나 요소 타입이 abstract·interface면
명시 codec 없이는 설정 오류가 난다.

### 3.2 다른 포맷의 serializer 등록

기본 codec 외의 포맷(Avro·Thrift 등)이 필요하면 메시지 serializer를 구현해 content type으로
등록한다. serializer의 책임은 업무 객체와 바이트 사이의 변환뿐이며, packet 이름 결정과 codec
선택은 Framework가 한다.

한 payload 타입에 **둘 이상이 해당하면 구성 오류**다. 타입 조건 없이 모든 타입을 받는 fallback
serializer는 하나만 두고, 타입 조건을 받는 serializer는 서로 겹치지 않게 여러 개 둘 수 있다.

### 3.3 명시 등록을 고르는 기준

명시 등록은 필요할 때만 하는 선택이다. 실시간 게임처럼 packet 크기와 인코딩 비용을 줄여야 하는
경우에 Protobuf codec을 등록하고 `.proto`로 DTO를 정의한다. 그 밖의 경우는 기본값을 사용한다.

## 4. Spot과 Actor의 handler 종류

무엇을 받느냐에 따라 구현할 interface가 다르다. 어느 것이든 `configure()`에서 등록한
것과 짝이 맞아야 한다.

받는 것마다 짝이 되는 interface와 등록 호출이 하나씩 있다.

| 받는 것 | 짝이 되는 등록 |
| --- | --- |
| Spot 앞 one-way packet | packet 등록 |
| Spot 앞 request | packet 등록 |
| Logical Multicast 구독 이벤트 | 구독 등록(channel과 topic 지정) |
| timer tick | timer 등록(이름과 주기 지정, [Timer와 worker](36-timer-worker.ko.md)) |
| member Actor 앞 one-way packet | actor packet 등록 |
| member Actor 앞 request | actor packet 등록 |

언어별 interface 이름과 등록 method는 다음과 같다.

| 받는 것 | 구현할 interface | 등록 |
| --- | --- | --- |
| Spot 앞 one-way packet | `ZLinkSpotPacketHandler<TSpot, TMessage>` | `addHandler(THandler.class)` |
| Spot 앞 request | `ZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `addHandler(THandler.class)` |
| Logical Multicast 구독 이벤트 | `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` | handler에 `@ZLinkSpotSubscription(topic)` + `addHandler(THandler.class)` |
| timer tick | `ZLinkSpotTimerHandler<TSpot>` | `context.addTimer(name, period, THandler.class, options)`([Timer와 worker](36-timer-worker.ko.md)) |
| member Actor 앞 one-way packet | `ZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | handler에 `@ZLinkSpotActorSend` + `addHandler(THandler.class)` |
| member Actor 앞 request | `ZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | handler에 `@ZLinkSpotActorRequest` + `addHandler(THandler.class)` |

**등록 method는 `addHandler` 하나다.** 무엇을 받는 handler인지는 구현한 interface와
annotation이 정한다. packet 이름은 message 타입의 `@ZLinkPacket`에서 온다.

handler는 대상 Spot instance를 첫 인자로 받는다. Spot 안에서 실행되므로 상태를 락 없이
직접 만진다.

> **샘플에서 보기 — [TicTacToe](../../../common/sample/tictactoe/README.ko.md).** 방 안의
> player가 수를 두는 handler다. member Actor 앞 request를 Spot과 Actor를 함께 받아
> 처리한다. 저장소의 실제 코드다.

```java
--8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/spots/tictactoegamespot/handlers/PlayActorPlaceMarkHandler.java:doc-actor-packet-handler"
```

최소 형태로 보면 이렇다.

```java
// Spot 앞 packet — 첫 인자가 대상 Spot instance다.
public final class ChatHandler implements ZLinkSpotPacketHandler<GameRoom, Chat> {
    @Override
    public CompletionStage<Void> handle(GameRoom spot, Chat message) {
        // Spot 상태를 직접 만진다. 락은 필요 없다.
        spot.appendChat(message.text());
        return CompletableFuture.completedFuture(null);
    }
}

// Spot 앞 request — 반환값이 reply다.
public final class GetRoomStateHandler
    implements ZLinkSpotRequestHandler<GameRoom, GetRoomState, RoomState> {
    @Override
    public CompletionStage<RoomState> handle(GameRoom spot, GetRoomState request) {
        return CompletableFuture.completedFuture(spot.snapshot());
    }
}

// 구독 이벤트 — @ZLinkSpotSubscription의 topic으로 들어온다.
public final class ScoreHandler
    implements ZLinkSpotSubscriptionHandler<GameRoom, ScoreChanged> {
    @Override
    public CompletionStage<Void> handle(GameRoom spot, ScoreChanged event) {
        spot.applyScore(event);
        return CompletableFuture.completedFuture(null);
    }
}

// member Actor 앞 packet — Spot과 Actor를 함께 받는다.
public final class PlaceMarkHandler
    implements ZLinkSpotActorSendHandler<GameRoom, PlayerActor, PlaceMark> {
    @Override
    public CompletionStage<Void> handle(
        GameRoom spot,
        // 이 메시지를 받은 Actor다.
        PlayerActor actor,
        ZLinkMessageContext messageContext,
        PlaceMark message) {
        spot.place(actor.actorId(), message.cell());
        return CompletableFuture.completedFuture(null);
    }
}
```

Actor 앞 request는 actor request handler이며
같은 인자에 반환값이 reply라는 점만 다르다.

`configure()`에서 handler를 등록하고 lifecycle callback에서 초기화와 정리를 수행한다.

```java
public final class GameRoom implements ZLinkSpot {
    private final ZLinkSpotContext context;

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        // Spot send handler를 등록한다.
        context.handlers().addHandler(ChatHandler.class);
        // 구독 topic은 ScoreHandler에 붙인 @ZLinkSpotSubscription이 정한다.
        context.handlers().addHandler(ScoreHandler.class);
    }

    @Override
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        CreateGame create = request.decode(CreateGame.class);
        boolean known = "ranked".equals(create.mode()) || "casual".equals(create.mode());
        return CompletableFuture.completedFuture(known
            ? ZLinkSpotCreateResponse.accept(new GameCreated(create.mode()))
            : ZLinkSpotCreateResponse.reject(new InvalidMode(create.mode())));
    }

    @Override
    public CompletionStage<Void> onInitialize() {
        // 생성 승인 뒤 메시지를 받기 전에 필요한 준비를 끝낸다.
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
        // Deadline까지 application resource를 정리한다.
        return CompletableFuture.completedFuture(null);
    }
}
```

`onClosing`의 reason은 explicit close, host shutdown, relocation out을 구분한다.
Framework는 `Deadline`이 끝날 때 cleanup token을 취소한다.

**세 이유가 Spot 종류마다 다 오는 것은 아니다.**

| 종료 이유 | Entry | User | Instance | 언제 |
| --- | :---: | :---: | :---: | --- |
| explicit close | X | O | O | application이 close를 시작해 local instance를 정상 정리할 때 |
| host shutdown | O | O | O | relocation 없이 host가 local Spot을 정리할 때 |
| relocation out | X | O | O | owner를 target으로 commit한 뒤 source instance를 정리할 때 |

**불리지 않는 두 자리를 기억한다.**

- **close가 실패하면 부르지 않는다.** User Spot에 Actor membership이 남아 있어 explicit
  close가 실패로 끝나면 `onClosing`은 실행되지 않는다. close 결과를 확인하지 않고
  "정리됐겠지" 하고 넘어가면 안 되는 이유다.
- **Actor가 떠나도 Entry Spot은 닫히지 않는다.** Actor 하나가 다른 Entry Spot으로
  옮겨가는 것은 Spot instance의 종료가 아니므로 Entry Spot의 `onClosing`을 부르지 않는다.

**Entry Spot 자체는 옮겨가지 않는다.** relocation out이 Entry Spot에 오지 않는 이유가
여기 있다. host를 옮길 때 Framework가 옮기는 것은 **Entry Spot에 속한 Actor**이고,
target 쪽 Entry Spot은 그 host가 시작할 때 새 ID와 수명으로 이미 만들어져 있다. 그래서
Entry Spot에 담아 둔 상태는 host를 옮겨도 따라가지 않는다 — **옮겨야 하는 상태는 Actor나
User Spot에 둔다.**

host shutdown에서는 **Actor membership과 local instance가 아직 살아 있는 상태로**
callback이 실행된다. 정리는 callback이 끝난 뒤에 이뤄지므로, 이 안에서 member Actor를
읽는 코드가 성립한다.

## 5. 관련 문서

- handler 하나를 만들어 호출까지 — [Channel 메시징](20-channel-messaging.ko.md)
- 패턴별 배선과 대상 선택 — [Channel 동작 원리](30-channel-patterns.ko.md)
- dispatch 실패 관측 — [모니터링](26-monitoring.ko.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
