---
title: "packet 송신 · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/stream-connector/04-sending.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# packet 송신

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Connector 옵션](03-connector-options.ko.md) | [다음: packet 수신](05-receiving.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/stream-connector/04-sending.ko.md) · [C#/.NET](../../../dotnet/guide/stream-connector/04-sending.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/stream-connector/04-sending.ko.md) · [Node/TypeScript](../../../node/guide/stream-connector/04-sending.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    응답을 기다리는 송신과 기다리지 않는 송신을 구분해 보내고, packet 이름과 metadata를
    원하는 값으로 정할 수 있다.

송신 호출은 값을 바로 보내지 않고 builder를 돌려준다. 이름·metadata·압축·timeout을 정한 뒤
**종결자를 호출해야 전송이 시작된다.** 종결자를 호출하지 않은 builder는 아무 일도 하지 않는다.

## 1. send — 응답을 기다리지 않는 송신

위치 갱신이나 입력처럼 서버의 답이 필요 없는 packet은 send로 보낸다. 종결자는 전송의 완료와
실패만 전달하며, 서버가 그 packet으로 무엇을 했는지는 담지 않는다.

```java
connector.send(new PositionUpdate(102.5f, 0.0f, -44.3f))
    .packetName("player.position")
    .submit()
    .toCompletableFuture()
    .join();
```

## 2. request — 응답을 기다리는 송신

로그인이나 조회처럼 서버의 답이 필요한 packet은 request로 보낸다. 응답은 packet 이름이 아니라
request마다 부여되는 sequence로 맞춰지므로, 여러 request를 동시에 보내도 응답이 도착하는 순서와
무관하게 각각 완료된다.

호출마다 timeout을 지정할 수 있고, 지정하지 않으면 connector의 기본 request timeout을 사용한다.

```java
LoginReply reply = connector.request(new LoginRequest("player-1", "tok-abc123"))
    .packetName("auth.login")
    .timeout(Duration.ofSeconds(5))
    .submit(LoginReply.class)
    .toCompletableFuture()
    .join();

long sessionId = reply.sessionId();
```

연결이 끊기면 대기 중이던 request는 모두 실패한다. 재연결한 뒤에도 자동으로 다시 보내지 않으므로,
다시 보내야 하는 request는 application이 판단해 보낸다.

## 3. packet 이름 결정

서버는 packet 이름으로 handler를 고른다. 이름은 다음 순서로 정해진다.

1. 호출자가 builder에 명시한 이름
2. payload 타입에 붙인 이름
3. payload 타입의 단순 이름

namespace나 package 한정자는 붙지 않는다. 컴파일러가 만드는 이름처럼 빌드 환경에 따라 달라지는
값도 사용하지 않는다 — 같은 타입을 보내도 서버가 handler를 찾지 못하기 때문이다.

같은 이름을 여러 곳에서 보낸다면 타입에 이름을 붙여 두는 편이 낫다. 그러면 호출하는 자리마다
이름을 반복하지 않는다.

```java
@ZLinkStreamPacketName("order.changed")
public record OrderChanged(String orderId) {
}
```

외부 protocol과 연동하느라 이미 encode한 payload를 보낼 때만 호출 자리에서 이름을 명시한다.

## 4. metadata

metadata는 packet에 붙이는 key-value다. trace id, locale, client 버전처럼 payload에 넣기 애매한
작은 값을 위한 자리다. 전송 시점의 값이 그대로 복사되므로, 보낸 뒤 원본을 고쳐도 전송된 packet은
바뀌지 않는다.

```java
connector.send(new ChatMessage("room-42", "hello"))
    .metadata("x-locale", "ko-KR")
    .metadata("x-client-version", "2.4.1")
    .submit()
    .toCompletableFuture()
    .join();
```

metadata 전체는 1024 bytes를 넘을 수 없고 이 한도는 option으로 조절하지 않는다. 같은 key를 두 번
담거나 빈 key를 담은 packet도 거부된다. 큰 값은 payload로 보낸다.

## 5. 압축 요청

압축 알고리즘은 connector를 만들 때 정하지만, 그 설정만으로 모든 packet이 압축되지는 않는다.
**압축을 명시한 송신만 압축한다.** 지도 청크처럼 큰 payload에만 지정하면 작은 packet에 압축
비용을 치르지 않는다.

```java
connector.send(new WorldChunk(chunkData))
    .packetName("world.chunk")
    .compress()
    .submit()
    .toCompletableFuture()
    .join();
```

압축을 끈 구성에서 압축을 지정하면 그 송신은 실패한다. 압축은 payload에만 적용하고 header에는
적용하지 않는다.

## 6. codec

payload를 bytes로 바꾸는 codec은 connector를 만들 때 하나를 정한다. 호출마다 codec을 고르는
표면은 없으므로, 송신 코드는 payload 타입과 이름만 다룬다. 기본 codec은 JSON이고 MessagePack과
Protobuf는 선택 package가 구현을 제공한다.

이미 encode된 bytes를 그대로 보내야 하는 연동에서는 payload가 지정한 codec 번호를 그대로 사용한다.

## 7. 크기 한도

송신 payload가 한도를 넘으면 **transport에 쓰기 전에** 실패한다. 연결은 그대로 유지되므로 그
호출만 실패하고 다른 packet은 영향을 받지 않는다. 압축을 지정한 송신은 압축한 결과를 한도와
비교한다.

64KB보다 큰 payload를 정상적으로 주고받아야 하면 [Connector 옵션](03-connector-options.ko.md)에서
송신 한도를 명시적으로 키운다.

## 8. 다음 장

- 서버가 보내는 packet 받기 — [packet 수신](05-receiving.ko.md)
- 송신이 실패하는 자리와 코드 — [오류 처리](07-error-handling.ko.md)
