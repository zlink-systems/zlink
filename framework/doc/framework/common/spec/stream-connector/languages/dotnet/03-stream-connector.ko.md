<!-- framework-adapter-nav:start -->
[문서 목록](../../../../../../README.ko.md) | [이전: .NET 시스템 구조](../../../server/languages/dotnet/interfaces/02-configuration-host.ko.md)
<!-- framework-adapter-nav:end -->

[.NET spec 목차](../../../server/languages/dotnet/README.ko.md)

# .NET Stream Connector 공개 계약

> 이 문서는 [Stream Connector 공통 스펙](../../32-stream-connector.ko.md)의 **`.NET` 투영**이다.
> **대상 실행 환경, transport, wire 계약, packet 모델, 연결 생명주기, 오류 의미, 기본값은 공통
> 스펙이 소유한다.** 이 문서는 그 의미가 `.NET`에서 갖는 **정확한 public 표면**만 고정한다.
>
> 사용법은 [.NET Stream Connector 가이드](../../../../../dotnet/guide/stream-connector/INDEX.ko.md)가
> 소유한다.

## 1. Package와 경계

공개 package는 `Zlink.Stream.Connector`다. **ASP.NET Core host, Spot, actor, location
runtime에 의존하지 않는다.**

정확한 member 목록과 배포 archive는 고정 snapshot이 소유한다.

- [API snapshot](../../../../../../../languages/dotnet/contract/api/Systems.Zlink.Stream.Connector.api.txt)
- [package snapshot](../../../../../../../languages/dotnet/contract/packages/Zlink.Stream.Connector.package.txt)

이 문서는 [snapshot](../../../server/00-foundation/02-glossary.ko.md#snapshot)의 member를 반복해 나열하지 않고 **표면의 구조와 `.NET` 고유 의미**를 고정한다.
검증 절차는 [이 문서 §13](#13-회귀-테스트)가 소유한다.

**담당 대상은 네이티브 빌드다**(데스크톱·서버, Unity, Godot C#). Unity 네이티브 빌드는 별도
package 없이 같은 `Zlink.Stream.Connector` NuGet package를 사용한다. **웹(브라우저·WASM)
빌드는 담당하지 않는다**([공통 스펙 §2](../../32-stream-connector.ko.md)).

## 2. 진입점

```csharp
public static class ZlinkStreamConnectorFactory
{
    public static IZlinkStreamConnector Create(ZlinkStreamConnectorOptions options);
}
```

**구현 타입은 숨긴다. factory가 public interface를 반환한다.**

## 3. `IZlinkStreamConnector`

```csharp
public interface IZlinkStreamConnector : IAsyncDisposable
{
    bool IsConnected { get; }
    ZlinkStreamConnectionState State { get; }
    ZlinkStreamCloseReason? CloseReason { get; } // 마지막 종료 사유. 끊긴 적이 없으면 null(§10)
    ZlinkStreamConnectorOptions Options { get; }
    int PendingDispatchCount { get; }
    int ReceivedCount(string name);             // packet 이름별 수신 개수(§8)

    IZlinkStreamLifecycleCall Connect { get; }
    IZlinkStreamLifecycleCall Close { get; }
    IZlinkStreamLifecycleCall Dispatch { get; }

    IZlinkStreamSendCall     Send(ZlinkStreamEncodedPayload payload);
    IZlinkStreamRequestCall  Request(ZlinkStreamEncodedPayload payload);
    IZlinkStreamWaitCall     WaitFor(string name);
    IZlinkStreamExpectNoneCall ExpectNone(string name);
    IZlinkStreamSequenceCall WaitForSequence(string name);
    IDisposable              On(string name, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler);


    IDisposable OnRequestSending(Action<ZlinkStreamRequestSendingContext> handler);
    IDisposable OnReplyReceived(Func<ZlinkStreamReplyReceivedContext, CancellationToken, ValueTask> handler);
    IDisposable OnConnectionStateChanged(Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask> handler);
    IDisposable OnDisconnected(Func<ZlinkStreamDisconnected, CancellationToken, ValueTask> handler);
    IDisposable OnErrorReceived(Func<ZlinkStreamError, CancellationToken, ValueTask> handler);

    IReadOnlyList<IZlinkStreamActor> Actors { get; }        // 지금 bind되어 있는 Actor handle(§3.1)
    IZlinkStreamActor? Actor(string actorId);               // 그 id의 열린 handle. 없으면 null
    IDisposable OnActorBound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler);
    IDisposable OnActorUnbound(Func<IZlinkStreamActor, CancellationToken, ValueTask> handler);
}
```

### 3.1 `IZlinkStreamActor`

[공통 스펙 §5.6](../../32-stream-connector.ko.md#56-bound-actor)의 Actor handle이다. connector가
`$zlink.actor.bound`로 만들고 `$zlink.actor.unbound`로 닫으며, application이 만들지 않는다.

```csharp
public interface IZlinkStreamActor
{
    string ActorId { get; }
    bool IsBound { get; }                                    // unbound 통지 뒤 false

    IZlinkStreamSendCall    Send(ZlinkStreamEncodedPayload payload);     // 이 Actor의 slot을 싣는다
    IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload);
    IDisposable On(string name, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler); // 이 Actor가 상대인 message만
}
```

- `Send`·`Request`의 builder는 connector 수준과 같은 `IZlinkStreamSendCall`·`IZlinkStreamRequestCall`이다
  (§4). `IsBound == false`인 handle의 `Async()`·`Submit(...)`은 `ValidationFailed`로 끝난다.
- connector 수준 `On(name, …)`은 Actor와 무관하게 모든 message를 받고, handle의 `On`은 그 Actor가
  상대인 message만 받는다. 둘 다 등록한 handler는 둘 다 실행된다.
- `OnActorBound`·`OnActorUnbound`는 다른 등록 표면과 같은 `IDisposable` 규칙을 따른다.
- typed 표면(§5)은 `IZlinkStreamActor`에도 같은 확장 메서드(`Send<TPayload>`, `Request<TPayload>`,
  `On<TPayload>`)를 제공한다.

- **handler는 등록 순서대로 호출된다.** handler 실패는 connector runtime을 종료하지 않고
  `UserCallbackFailed` 오류로 보고한다.
- **연결 이벤트는 C# `event`가 아니라 등록 메서드다.** `event`는 등록 시점에 해제 값을
  돌려주지 않아 공통 스펙 §7을 만족하지 못한다. 화면 하나의 수명에 맞춰 구독을 관리하는
  client가 등록한 델리게이트를 따로 보관해야 하기 때문이다.
- `PendingDispatchCount`는 **dispatch pump 상태를 진단하기 위한 값**이다.
  **application flow control에 사용하지 않는다.**
- `ReceivedCount(name)`은 packet 이름별 수신 개수를 돌려준다. 집계와 초기화는
  [공통 스펙 §10](../../32-stream-connector.ko.md#10-수신-메시지-큐)이 정한다.
- **등록 표면은 모두 `IDisposable`을 돌려준다**([공통 스펙 §7](../../32-stream-connector.ko.md#7-dispatch-모드)).
  `On(...)`과 `OnConnectionStateChanged`·`OnDisconnected`·`OnErrorReceived`가 같다.
  `Dispose()`를 두 번 호출해도 오류로 처리하지 않는다.

`.NET`은 오류를 `ZlinkStreamError`로 전달하고, 던지는 표면은 그 값을 `ZlinkStreamException`에
담는다([공통 스펙 §9.2](../../32-stream-connector.ko.md#92-전달--받는-쪽이-코드를-읽을-수-있어야-한다)).
호출자는 `ZlinkStreamException.Error.Code`로 오류 코드를 읽는다.

```csharp
public sealed record ZlinkStreamError(
    ZlinkStreamErrorCode Code,      // 공통 스펙 §9의 닫힌 13개 코드
    string Message,
    Exception? Exception = null);   // 원인 예외. 없으면 null

public sealed class ZlinkStreamException(ZlinkStreamError error)
    : Exception(error.Message, error.Exception)
{
    public ZlinkStreamError Error { get; } = error; // 코드를 읽는 자리다
}
```

## 4. Call builder

**packet name과 metadata는 payload 객체가 아니라 operation builder가 소유한다.**

```csharp
public interface IZlinkStreamLifecycleCall
{
    ValueTask Async(CancellationToken cancellationToken = default);
}

public interface IZlinkStreamSendCall
{
    IZlinkStreamSendCall PacketName(string name);
    IZlinkStreamSendCall Metadata(string key, string value);
    IZlinkStreamSendCall Metadata(ZlinkStreamMetadata metadata);
    IZlinkStreamSendCall Compress();
    ValueTask Async(CancellationToken cancellationToken = default); // 비동기 완료와 실패만 전달한다.
}

public interface IZlinkStreamRequestCall
{
    IZlinkStreamRequestCall PacketName(string name);
    IZlinkStreamRequestCall Metadata(string key, string value);
    IZlinkStreamRequestCall Metadata(ZlinkStreamMetadata metadata);
    IZlinkStreamRequestCall Compress();
    IZlinkStreamRequestCall Timeout(TimeSpan timeout);
    ValueTask<ZlinkStreamEncodedPayload> Async(CancellationToken cancellationToken = default);
    void Submit(Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback);
    void Submit(Action<ZlinkStreamResult> callback);
}

public interface IZlinkStreamWaitCall
{
    // Timeout(...), Where(...) 로 이 wait의 제한과 predicate를 정한다.
    ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> Async(CancellationToken cancellationToken = default);
}
```

- **`Send`는 reply를 기다리지 않는 one-way 전송이다.** `Async()`의 완료 값에는 전송 결과나
  admission status가 없으며, 비동기 완료와 실패만 전달한다(§6).
  응답이 필요하면 `Request`를 사용한다.
- **`Timeout(...)`은 그 operation에만 적용한다.**
- **`On(...)`은 지속적인 push handler, `WaitFor(...)`는 한 번성 대기**다. production의 push 처리는
  `On(...)`, sample·CLI·E2E의 대기는 `WaitFor(...)`를 사용한다.
- **`Metadata`는 전송 시점에 불변 snapshot으로 복사된다.**

### 4.1 요청 hook

[공통 스펙 §5.7](../../32-stream-connector.ko.md#57-요청-hook)의 두 hook을 `OnRequestSending`·`OnReplyReceived`(§3)와 다음 context로 투영한다. 송신 hook은 dispatch mode를 따르지 않으므로 동기 `Action`이고, 응답 hook은 다른 수신 callback과 같은 형태다.

```csharp
public sealed class ZlinkStreamRequestSendingContext
{
    public string RequestPacketName { get; }
    public string? ActorId { get; }
    public void SetMetadata(string key, string value);
}

public sealed class ZlinkStreamReplyReceivedContext
{
    public string RequestPacketName { get; }
    public string? ActorId { get; }
    public bool Succeeded { get; }
    public ZlinkStreamMessage<ZlinkStreamEncodedPayload>? Reply { get; }
    public ZlinkStreamError? Error { get; }
    public TimeSpan Elapsed { get; }
}
```

## 5. Typed 표면

공통 스펙 §5의 이름 두 형태는 connector와 Actor handle의 typed `On`, connector의 `WaitFor`·`ExpectNone`·`WaitForSequence`에서 packet 이름을 받는 overload와 받지 않는 overload로 나타난다. typed `Send`·`Request`는 반환된 builder의 `PacketName(string)`으로 이름을 명시한다.

`ZlinkStreamTypedConnectorExtensions`가 `Send<TPayload>`, `Request<TPayload>`, `On<TPayload>`,
`WaitFor<TPayload>`, `ExpectNone<TPayload>`, `WaitForSequence<TPayload>`를 제공하고, 각각 typed
builder를 반환한다.

**packet identity는 `IZlinkStreamPacketNameResolver`가 결정한다.**
[공통 스펙 §5](../../32-stream-connector.ko.md#5-packet-모델)가 요구하는 타입에 packet 이름을 붙이는
수단은 attribute다. 기본 resolver는 이 attribute를 우선하고, 없으면 타입 이름을 사용한다.

```csharp
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
public sealed class ZlinkStreamPacketNameAttribute(string name) : Attribute
{
    public string Name { get; } = name; // 타입에 붙인 packet 이름
}
```

- **operation별 `PacketName(...)` override를 허용한다.** 이미 encode한 raw payload와 외부 protocol
  interop을 위해서다. **이는 server framework의 typed registration descriptor와 역할이 다르며,
  server handler call site에 packet 이름을 다시 노출하는 근거가 아니다.**
- **typed decode 이후에도 connector 내부 buffer나 mutable transport header를 공개하지 않는다.**
- **raw header 객체를 public API에 노출하지 않는다.**

codec 표면은 `IZlinkStreamPayloadCodec`과 `IZlinkStreamCompressionCodec`이다. `ZlinkStreamJsonCodec`이
기본 payload codec이며, `CompressionCodec`을 지정하면 built-in 대신 그 구현을 사용한다.

[공통 스펙 §5.4](../../32-stream-connector.ko.md#54-codec)의 두 주입점은
`ZlinkStreamConnectorOptions`의 다음 property다.

```csharp
public IZlinkStreamPayloadCodec? PayloadCodec { get; init; }          // null이면 ZlinkStreamJsonCodec
public IZlinkStreamPacketNameResolver NameResolver { get; init; }
    = new ZlinkStreamPacketNameResolver();                            // 기본 resolver
```

Framework codec extension이 STREAM header 값을 함께 제공해야 하면 Stream Connector package의
`IZlinkStreamCodecRegistration`을 구현한다. 이 descriptor는 STREAM 전용 정보만 소유한다. 공통 serializer
registry는 STREAM enum이나 compression package를 참조하지 않는다.

```csharp
public interface IZlinkStreamCodecRegistration
{
    string ContentType { get; }
    ZlinkStreamCodec Codec { get; }
}
```

## 6. Lifecycle과 완료 의미

**`.NET` 고유 계약이다.** 상태 전이 자체는 [공통 스펙 §6](../../32-stream-connector.ko.md)이 소유한다.

- `Connect.Async(...)`는 **연결과 receive loop 준비가 끝나면** 완료된다.
- `Close.Async(...)`의 완료 조건은 [공통 스펙 §7](../../32-stream-connector.ko.md)이 정한다.
- **반복된 `Close`와 `DisposeAsync()`는 같은 terminal 결과 또는 실패를 공유한다.**
- **callback 안에서 `DisposeAsync()`로 자기 callback의 종료를 기다리는 순환 대기는 허용하지 않고
  즉시 오류로 처리한다.**
- **lifecycle waiter의 `CancellationToken`은 그 waiter만 취소한다.** 이미 시작된 공유 종료 작업을
  취소하지 않는다.
- **frame write가 시작된 뒤에는 caller cancellation이 partial frame을 만들지 않는다.**
- **caller가 취소한 operation은 그 `CancellationToken`의 `OperationCanceledException`으로 끝난다**
  ([공통 스펙 §5.2](../../32-stream-connector.ko.md#52-request-correlation)).

## 7. Dispatch

`.NET`은 [공통 스펙 §7](../../32-stream-connector.ko.md#7-dispatch-모드)의 `Manual` pump를
`Dispatch.Async(...)`로 표현한다. Outbound admission과 순서, timeout은
[공통 스펙 §5.2](../../32-stream-connector.ko.md#52-request-correlation)가 정한다.

## 8. 수신 메시지 history

`On(...)` handler가 등록된 이름의 message는 dispatch가 handler snapshot을 인수하면 unread 기록에
남기지 않는다. handler가 없는 이름의 message는 unread 기록에 남고 `WaitFor(...)`가 하나씩 소비한다.
response와 heartbeat 같은 control frame은 이 기록을 거치지 않는다.

### 8.1 테스트 대기 표면

계약은 [공통 스펙 §10](../../32-stream-connector.ko.md#10-수신-메시지-큐)가 소유한다. `.NET` 표면은 다음과 같다.

**push 관측 — connector 메서드**(§4의 `WaitFor`와 같은 자리). 각각 typed builder를 반환한다.

```csharp
IZlinkStreamWaitCall       WaitFor(string name);        // 도달할 때까지 대기
IZlinkStreamExpectNoneCall ExpectNone(string name);     // .Within(window) 동안 오지 않는지
IZlinkStreamSequenceCall   WaitForSequence(string name); // .Expect(p).Expect(p)…를 순서대로
```

typed 표면은 `ZlinkStreamTypedConnectorExtensions`의 확장 메서드다. 각 표면은
**packet 이름을 `TPayload`에서 결정하는 overload와 호출자가 명시하는 overload를 함께** 둔다
([공통 스펙 §10.1.1](../../32-stream-connector.ko.md#1011-push-관측-표면--waitfor-계열)). 이름을 주지
않으면 `Options.NameResolver`가 `typeof(TPayload)`에서 이름을 결정한다.

```csharp
public static ZlinkStreamTypedWaitBuilder<TPayload>       WaitFor<TPayload>(this IZlinkStreamConnector connector);
public static ZlinkStreamTypedWaitBuilder<TPayload>       WaitFor<TPayload>(this IZlinkStreamConnector connector, string name);
public static ZlinkStreamTypedExpectNoneBuilder<TPayload> ExpectNone<TPayload>(this IZlinkStreamConnector connector);
public static ZlinkStreamTypedExpectNoneBuilder<TPayload> ExpectNone<TPayload>(this IZlinkStreamConnector connector, string name);
public static ZlinkStreamTypedSequenceBuilder<TPayload>   WaitForSequence<TPayload>(this IZlinkStreamConnector connector);
public static ZlinkStreamTypedSequenceBuilder<TPayload>   WaitForSequence<TPayload>(this IZlinkStreamConnector connector, string name);
```

negative observation과 순서 검증의 typed builder는 다음 public interface를 고정한다.

```csharp
public sealed class ZlinkStreamTypedExpectNoneBuilder<TPayload>
{
    // 이 packet이 도착하지 않아야 하는 관찰 구간을 정한다.
    public ZlinkStreamTypedExpectNoneBuilder<TPayload> Within(TimeSpan window);
    public ValueTask Async(CancellationToken cancellationToken = default);
}

public sealed class ZlinkStreamTypedSequenceBuilder<TPayload>
{
    // 도착 순서대로 적용할 다음 typed predicate를 추가한다.
    public ZlinkStreamTypedSequenceBuilder<TPayload> Expect(
        Func<ZlinkStreamMessage<TPayload>, bool> predicate);
    public ZlinkStreamTypedSequenceBuilder<TPayload> Timeout(TimeSpan timeout);
    public ValueTask<IReadOnlyList<ZlinkStreamMessage<TPayload>>> Async(
        CancellationToken cancellationToken = default);
}
```

- `ExpectNone(name).Within(TimeSpan).Async(ct)` — window 안에 도착하면 **`ValidationFailed`를 담은 `ZlinkStreamException`을 던진다**. `WaitFor`의 대칭.
- `WaitForSequence(name).Expect(p1).Expect(p2)…Timeout(t).Async(ct)` — `IReadOnlyList<ZlinkStreamMessage<TPayload>>`를 반환하며, 순서 관측과 실패는 [공통 스펙 §10.1](../../32-stream-connector.ko.md#101-테스트-대기-표면)이 정한다.
- **술어와 반환은 `ZlinkStreamMessage<TPayload>`를 다룬다.** `Where(...)`와 `Expect(...)`가 받는 인자도 payload가 아니라 message다.
- **status 전용 표면을 두지 않는다.** status는 payload 필드이므로 `WaitFor<T>(name).Where(p => p.Status == …)`로 표현한다. connector가 어느 필드가 status인지 알지 않는다.

- **도메인 REST 폴링(`GET /deliveries/{id}` 등)은 이 표면이 아니다.** 그건 `ZLinkHttpClient`의 일이다.

## 9. Transport와 TLS

scheme → transport 매핑은 [공통 스펙 §3.1](../../32-stream-connector.ko.md)이 소유한다. `.NET`은 이를
`ZlinkStreamTransport` enum(`Tcp`, `Tls`, `WebSocket`, `WebSocketSecure`)으로 표현한다.

- **nullable `Transport` option은 transport를 고르는 경로가 아니다.** URI scheme과 설정이 일치하는지
  확인하는 **보조 값**이며, 어긋나면 `ConfigurationError`로 실패한다.
- **TLS와 WSS는 기본적으로 인증서 chain과 host name을 검증한다.**
  `SkipServerCertificateValidation`의 기본값은 `false`이며 **테스트의 자체 서명 인증서에만**
  사용한다.

## 10. 종료 사유

값 집합과 의미는 [공통 스펙 §6.2](../../32-stream-connector.ko.md#62-종료-사유)가 소유한다. `.NET`은
`ZlinkStreamCloseReason` enum으로 표현한다.

**읽기 표면은 `IZlinkStreamConnector.CloseReason` property다**(§3). 타입은
`ZlinkStreamCloseReason?`이며, 한 번도 끊긴 적이 없으면 `null`이다. `Disconnected` event의 인자
`ZlinkStreamDisconnected.CloseReason`은 이 property에 추가하는 표면이다.

**`session-closing` frame의 wire 값은 1~6이고 `.NET` enum의 내부 ordinal은 0~5다.** codec이 둘을
명시적으로 변환하므로 **enum을 정수로 cast해 wire 값으로 사용하지 않는다.**

수신 한도 위반의 terminal 여부, 종료 사유와 reconnect 조건은
[공통 스펙 §9](../../32-stream-connector.ko.md#9-오류-의미)이 소유한다. `.NET`은 그 오류를
`ZlinkStreamErrorCode.FrameTooLarge`로 표현한다.

## 11. Flow

Connector는 flow를 만들거나 보내거나 노출하거나 전파하지 않는다([공통 스펙 §5.5](../../32-stream-connector.ko.md#55-flow)). 수신 message는 Actor 식별자만 추가로 노출한다.

```csharp
public sealed record ZlinkStreamMessage<TPayload>(
    string Name,
    ZlinkStreamMetadata Metadata,
    TPayload Payload,
    string? ActorId = null);
```

## 12. Options와 검증

**기본값은 [공통 스펙 §6.1](../../32-stream-connector.ko.md)이 소유한다.** `.NET`은 이를
`ZlinkStreamConnectorOptions`(+ `ZlinkStreamHeartbeatOptions`, `ZlinkStreamReconnectOptions`)의
property로 표현한다.

[공통 스펙 §6](../../32-stream-connector.ko.md#6-연결-생명주기)이 요구하는 **무제한 reconnect는
nullable `int`의 `null`로 표현한다.**

```csharp
public int? MaxAttempts { get; init; } = 3; // null은 무제한
```

`ZlinkStreamConnectorFactory.Create(options)`는 [공통 스펙 §6.3](../../32-stream-connector.ko.md#63-옵션-검증)의
option 검증에 실패하면 connector를 만들지 않고 해당 오류 코드를 담은 `ZlinkStreamException`을 던진다.

## 13. 회귀 테스트

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `StreamConnectorTests.ConnectorImplementationIsHiddenBehindPublicInterface` | 구현 타입은 숨기고 [factory](../../../server/00-foundation/02-glossary.ko.md#factory)가 public interface를 반환한다. |
| `StreamConnectorTests.ConnectorCallInterfacesMatchTheFrozenSurface` | lifecycle, send, request와 wait call의 정확한 member를 고정한다. |
| `StreamConnectorTests.ConnectorOptionsMatchTheFrozenDefaults` | connector option의 기본값을 고정한다. |
| `StreamConnectorTests.ManualDispatchRunsHandlerOnDispatchCaller` | Manual callback은 dispatch caller에서 실행된다. |
| `StreamConnectorTests.ImmediateDispatchRunsHandlerWithoutManualDispatch` | Immediate callback은 별도 manual dispatch 없이 실행된다. |
| `StreamConnectorTests.ManualRequestCallbackAdmission_Is_Bounded_And_Never_Falls_Back_To_A_Background_Thread` | request callback admission은 bounded이며 background 우회를 허용하지 않는다. |
| `StreamConnectorTests.RequestTimeoutRemovesPendingRequest` | timeout 뒤 pending request를 제거한다. |
| `StreamConnectorTests.TcpTypedRequestCorrelatesResponse` | typed request와 response correlation을 유지한다. |
| `StreamConnectorTests.TypedConnectorUsesJsonByDefaultAndDecodeReply` | typed 기본 codec은 JSON이다. |
| `StreamConnectorTests.PacketNameAttributeIsUsedByDefault` | [packet name](../../../server/00-foundation/02-glossary.ko.md#packet-name) attribute를 기본 identity로 사용한다. |
| `StreamConnectorTests.DisconnectEventCarriesTheFrozenCloseReasonContract` | disconnect event의 닫힌 종료 사유를 고정한다. |
| `StreamConnectorTests.SessionClosingPublishesServerDrainReasonAfterDisconnectedState` | session-closing frame을 `ServerDrain` 사유로 변환한다. |
| `StreamConnectorTests.SharedCloseFaultIsObservedByRepeatedCloseAndDispose` | 반복 close와 dispose가 같은 실패를 관찰한다. |
| `StreamConnectorTests.OneWayAsync_Waits_For_Bounded_Queue_Admission` | one-way terminal은 bounded queue 수락까지 비동기로 기다리고 결과값 없이 완료한다. |
| `StreamConnectorTests.RequestQueueWaitsForEarlierAcceptedOneWaySend` | 먼저 수락된 one-way send와 뒤 request의 wire 전송 순서를 보존한다. |
| `StreamConnectorTests.CallerCancellationDoesNotInterruptAnInProgressFrameWrite` | frame write가 시작된 뒤에는 caller cancellation이 partial frame을 만들지 않는다. |
| `StreamConnectorTests.ConnectorOutboundFramesNeverCarryFlowAndOnlyRequestsCarryCorrelation` | connector 송신에 flow flag가 없고 request에만 correlation id가 있다. |
| `StreamConnectorTests.RequestHooksRunInOrderAddWireMetadataAndIsolateFailures` | hook 등록 순서와 metadata 추가, callback 실패 격리를 확인한다. |
| `StreamConnectorTests.ReplyHookObservesRemoteFailureTimeoutAndClose` | reply hook이 원격 오류, timeout, 연결 종료를 관찰한다. |
| `StreamConnectorTests.ManualSendingHookRunsOnRequestCallerAndReplyHookWaitsForDispatch` | Manual mode에서도 송신 hook은 요청 호출 스레드에서 실행하고 metadata는 dispatch 없이 전송한다. 응답 hook은 dispatch를 기다린다. |
| `StreamConnectorTests.ActorHandlersIsolateMatchingNamesAndRequestHooksReportActorId` | Actor 수신 등록은 같은 이름의 다른 Actor 메시지를 받지 않고 hook은 Actor ID를 받는다. |
| `StreamConnectorTests.HeaderProtocolEnforcesControlPacketContract` | control packet의 codec·flag·payload 계약을 고정한다. |

Release 검증은 `scripts/verify_packaged_contract.sh`로 source assembly, API snapshot, 실제 NuGet
package와 clean consumer가 모두 같은 공개 계약인지 확인한다.

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../../../../README.ko.md)
<!-- framework-adapter-nav:bottom:end -->
