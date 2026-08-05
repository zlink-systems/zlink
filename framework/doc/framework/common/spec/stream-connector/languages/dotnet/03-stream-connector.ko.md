<!-- framework-adapter-nav:start -->
[문서 목록](../../../../../../README.ko.md) | [이전: .NET 시스템 구조](../../../server/languages/dotnet/01-system-structure.ko.md)
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

공개 package는 `Systems.Zlink.Stream.Connector`다. **ASP.NET Core host, Spot, actor, location
runtime에 의존하지 않는다.**

정확한 member 목록과 배포 archive는 고정 snapshot이 소유한다.

- [API snapshot](../../../../../../../languages/dotnet/contract/api/Systems.Zlink.Stream.Connector.api.txt)
- [package snapshot](../../../../../../../languages/dotnet/contract/packages/Systems.Zlink.Stream.Connector.package.txt)

이 문서는 [snapshot](../../../01-glossary.ko.md#snapshot)의 member를 반복해 나열하지 않고 **표면의 구조와 `.NET` 고유 의미**를 고정한다.
검증 절차는 [이 문서 §15](#15-회귀-테스트)가 소유한다.

**담당 대상은 네이티브 빌드다**(데스크톱·서버, Unity, Godot C#). Unity 네이티브 빌드는 별도
package 없이 같은 `Systems.Zlink.Stream.Connector` NuGet package를 사용한다. **웹(브라우저·WASM)
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
    ZlinkStreamConnectorOptions Options { get; }
    int PendingDispatchCount { get; }

    IZlinkStreamLifecycleCall Connect { get; }
    IZlinkStreamLifecycleCall Close { get; }
    IZlinkStreamLifecycleCall Dispatch { get; }

    IZlinkStreamSendCall     Send(ZlinkStreamEncodedPayload payload);
    IZlinkStreamRequestCall  Request(ZlinkStreamEncodedPayload payload);
    IZlinkStreamWaitCall     WaitFor(string name);
    IZlinkStreamExpectNoneCall ExpectNone(string name);
    IZlinkStreamSequenceCall WaitForSequence(string name);
    IDisposable              On(string name, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler);

    IDisposable ObserveInbound(Func<ZlinkStreamInboundObservation, CancellationToken, ValueTask> observer);

    event Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>? ConnectionStateChanged;
    event Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>?           Disconnected;
    event Func<ZlinkStreamError, CancellationToken, ValueTask>?                  ErrorReceived;
}
```

- **event handler는 등록 순서대로 호출된다.** handler 실패는 connector runtime을 종료하지 않고
  `UserCallbackFailed` 오류로 보고한다.
- `PendingDispatchCount`는 **dispatch pump 상태를 진단하기 위한 값**이다.
  **application flow control에 사용하지 않는다.**

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
  응답이 필요하면 `Request`를 쓴다.
- **`Timeout(...)`은 그 operation에만 적용한다.**
- **`On(...)`은 지속적인 push handler, `WaitFor(...)`는 한 번성 대기**다. production의 push 처리는
  `On(...)`, sample·CLI·E2E의 대기는 `WaitFor(...)`를 쓴다.
- **`Metadata`는 전송 시점에 불변 snapshot으로 복사된다.**

## 5. Typed 표면

`ZlinkStreamTypedConnectorExtensions`가 `Send<TPayload>`, `Request<TPayload>`, `On<TPayload>`,
`WaitFor<TPayload>`, `ExpectNone<TPayload>`, `WaitForSequence<TPayload>`를 제공하고, 각각 typed
builder를 반환한다.

**packet identity는 `IZlinkStreamPacketNameResolver`가 결정한다.** 기본 resolver는
`ZlinkStreamPacketNameAttribute`를 우선하고, attribute가 없으면 타입 이름을 사용한다.

- **operation별 `PacketName(...)` override를 허용한다.** 이미 encode한 raw payload와 외부 protocol
  interop을 위해서다. **이는 server framework의 typed registration descriptor와 역할이 다르며,
  server handler call site에 packet 이름을 다시 노출하는 근거가 아니다.**
- **typed decode 이후에도 connector 내부 buffer나 mutable transport header를 공개하지 않는다.**
- **raw header 객체를 public API에 노출하지 않는다.**

codec 표면은 `IZlinkStreamPayloadCodec`과 `IZlinkStreamCompressionCodec`이다. `ZlinkStreamJsonCodec`이
기본 payload codec이며, `CompressionCodec`을 지정하면 built-in 대신 그 구현을 사용한다.

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
- **callback 밖**의 `Close.Async(...)`는 연결 종료와 terminal callback 정리가 끝나면 완료된다.
- **callback 안**의 `Close.Async(...)`는 **순환 대기를 피하려고 종료를 시작한 뒤 즉시 반환한다.**
  이후 callback 밖의 `Close.Async(...)` 또는 `DisposeAsync()`가 공유 terminal 결과를 기다린다.
- **반복된 `Close`와 `DisposeAsync()`는 같은 terminal 결과 또는 실패를 공유한다.**
- **callback 안에서 `DisposeAsync()`로 자기 callback의 종료를 기다리는 순환 대기는 허용하지 않고
  즉시 오류로 처리한다.**
- **lifecycle waiter의 `CancellationToken`은 그 waiter만 취소한다.** 이미 시작된 공유 종료 작업을
  취소하지 않는다.
- **frame write가 시작된 뒤에는 caller cancellation이 partial frame을 만들지 않는다.**

## 7. Dispatch와 bounded admission

**`.NET` 고유 계약이다.**

| 항목 | 계약 |
|---|---|
| `Manual`(기본) | 수신 callback·request callback·lifecycle event가 **`Dispatch.Async(...)`를 호출한 실행 문맥**에서 처리된다 |
| `Immediate` | **receive 경로에서 인라인 실행한다**(별도 dispatch 작업 없음). 느린 handler는 receive loop를 막으므로 backpressure가 그대로 걸린다 |
| `MaxPendingDispatchCallbacks` | **`Manual`에서만 적용된다.** 수신 handler뿐 아니라 이미 수락된 request의 완료 callback을 보존할 예약 슬롯도 이 제한에 포함된다. `Immediate`는 큐를 거치지 않으므로 이 bounded admission을 우회한다 |
| outbound 전송 queue | dispatch 제한과 **별개인 순서 보존 queue**. 최대 **4096개** 전송을 보관하며, 넘치면 **즉시 오류**로 거부한다 |

- **먼저 수락한 send는 뒤에 시작한 request보다 먼저 전송된다.** request는 **자기 frame의 실제 write가
  끝난 뒤** response를 기다린다.
- **전송을 background thread의 callback 실행으로 우회하지 않는다.**

## 8. 수신 메시지 history

`WaitFor(...)`가 사용할 unread 수신 기록은 `MaxReceivedMessages`로 제한한다. **이 제한은 response와
heartbeat 같은 control frame의 처리를 막지 않는다.**

`On(...)` handler가 등록된 이름의 message도 공통 수신 메시지 큐의 admission을 거친다. dispatch가
handler snapshot을 인수하면 unread 기록에는 남기지 않는다. handler가 없는 이름의 message는 unread
기록에 남고 `WaitFor(...)`가 하나씩 소비한다. 따라서 `MaxReceivedMessages`는 dispatch 전 대기와 unread
기록을 함께 제한한다. inbound observer는 이 선택과 별도의 관찰 경로이므로 두 경우 모두 frame
snapshot을 받는다.

**큐가 가득 차면 새로 도착한 message를 거부하고 `ReceivedMessageDropped`를 보고한다**
([공통 스펙 §10.1](../../32-stream-connector.ko.md)).

### 8.1 테스트 대기 표면

계약은 [공통 스펙 §10.2](../../32-stream-connector.ko.md)가 소유한다. `.NET` 표면은 다음과 같다.

**push 관측 — connector 메서드**(§4의 `WaitFor`와 같은 자리). 각각 typed builder를 반환한다.

```csharp
IZlinkStreamWaitCall       WaitFor(string name);        // 도달할 때까지 대기
IZlinkStreamExpectNoneCall ExpectNone(string name);     // .Within(window) 동안 오지 않는지
IZlinkStreamSequenceCall   WaitForSequence(string name); // .Expect(p).Expect(p)…를 순서대로

// typed: ZlinkStreamTypedConnectorExtensions 가 WaitFor<T>·ExpectNone<T>·WaitForSequence<T> 제공
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

- `ExpectNone(name).Within(TimeSpan).Async(ct)` — window 안에 도착하면 **오류를 던진다**. `WaitFor`의 대칭.
- `WaitForSequence(name).Expect(p1).Expect(p2)…Timeout(t).Async(ct)` — 같은 이름 push가 **술어 순서대로** 도착하는지 확인하고 payload 목록을 돌려준다. "N개 도착"이 아니라 **"순서대로 도착"** 을 검증한다.
- **status 전용 표면을 두지 않는다.** status는 payload 필드이므로 `WaitFor<T>(name).Where(p => p.Status == …)`로 표현한다. connector가 어느 필드가 status인지 알지 않는다.

- **도메인 REST 폴링(`GET /deliveries/{id}` 등)은 이 표면이 아니다.** 그건 `ZLinkHttpClient`의 일이다.

## 9. Inbound observer

관찰 의미와 격리·overflow 규칙은 [공통 스펙 §10](../../32-stream-connector.ko.md)이 소유한다.
`.NET` 표면의 제약은 다음과 같다.

- `ObserveInbound(...)`는 **연결 시작 전에만** 등록하고 `IDisposable`을 반환한다.
- **observer callback에서 connector의 send·request·wait·dispatch를 호출하지 않는다.**
- **observer는 frame을 drop·변환·reply할 수 없다.**
- **`DisposeAsync()`는 cancellation을 무시하고, 실행 중인 observer가 끝날 때까지 기다린다.**

## 10. Transport와 TLS

scheme → transport 매핑은 [공통 스펙 §3.1](../../32-stream-connector.ko.md)이 소유한다. `.NET`은 이를
`ZlinkStreamTransport` enum(`Tcp`, `Tls`, `WebSocket`, `WebSocketSecure`)으로 표현한다.

- **nullable `Transport` option은 transport를 고르는 경로가 아니다.** URI scheme과 설정이 일치하는지
  확인하는 **보조 값**이며, 어긋나면 `ConfigurationError`로 실패한다.
- **TLS와 WSS는 기본적으로 인증서 chain과 host name을 검증한다.**
  `SkipServerCertificateValidation`의 기본값은 `false`이며 **테스트의 자체 서명 인증서에만**
  사용한다.

## 11. 종료 사유

값 집합과 의미는 [공통 스펙 §6.3](../../32-stream-connector.ko.md#63-종료-사유)가 소유한다. `.NET`은
`ZlinkStreamCloseReason` enum으로 표현하고 **`Disconnected` event의 인자
`ZlinkStreamDisconnected.CloseReason`으로 노출한다.**

**`session-closing` frame의 wire 값은 1~6이고 `.NET` enum의 내부 ordinal은 0~5다.** codec이 둘을
명시적으로 변환하므로 **enum을 정수로 cast해 wire 값으로 사용하지 않는다.**

수신 한도 위반의 terminal 여부, 종료 사유와 reconnect 조건은
[공통 스펙 §9](../../32-stream-connector.ko.md#9-오류-의미)이 소유한다. `.NET`은 그 오류를
`ZlinkStreamErrorCode.FrameTooLarge`, 종료 사유를 `ZlinkStreamCloseReason.TransportError`로 표현한다.

## 12. Flow

**connector outbound operation은 별도 public 옵션 없이 UUIDv7 `flow_id`를 한 번 생성한다.**
callback 안에서 시작한 후속 operation은 **현재 inbound flow를 재사용하고, callback이 끝나면 ambient
flow를 정리한다.**

wire 표현은 [공통 스펙 §4.2](../../32-stream-connector.ko.md)와
[flow-correlation](../../../27-flow-correlation.ko.md)이 소유한다.

## 13. Metric

connector metric은 [Stream Connector 공통 계약 §6.2](../../32-stream-connector.ko.md#62-connector-reconnect-계기)의
이름과 닫힌 label을 따른다. `.NET` connector는 `System.Diagnostics.Metrics` provider에
`zlink.stream.reconnects`를 게시하며 application과 E2E는 `MeterListener`로 읽는다. **metric listener 실패는
send/request 결과나 연결 상태를 바꾸지 않는다.**

## 14. Options와 검증

**기본값은 [공통 스펙 §6.1](../../32-stream-connector.ko.md)이 소유한다.** `.NET`은 이를
`ZlinkStreamConnectorOptions`(+ `ZlinkStreamHeartbeatOptions`, `ZlinkStreamReconnectOptions`)의
property로 표현한다.

공통 계약의 `MaxInboundObserverPayloadPreviewBytes`는 payload preview 길이를 byte 단위로 제한하며 기본값은
0이다. `.NET`은 이 공통 option을 같은 이름의 property로 투영한다.

**`.NET`에만 있는 option:**

| option | 기본값 | 의미 |
|---|---|---|
| `MaxPendingDispatchCallbacks` | 1024 | dispatch 대기 callback 한도(§7) |

**검증 계약:**

| 위반 | 실패 |
|---|---|
| endpoint 없음 | `ArgumentException` |
| 지원하지 않는 scheme, URI scheme과 `Transport` 불일치 | **연결을 시작하기 전에** `ZlinkStreamException`의 `ConfigurationError` |
| 유효하지 않은 timeout·queue 크기·heartbeat/reconnect 조합 | `ValidationFailed` |

모든 timeout과 queue 크기 option은 **양수**여야 하고, preview 길이는 **음수일 수 없다.**

## 15. 회귀 테스트

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `StreamConnectorTests.ConnectorImplementationIsHiddenBehindPublicInterface` | 구현 타입은 숨기고 [factory](../../../01-glossary.ko.md#factory)가 public interface를 반환한다. |
| `StreamConnectorTests.ConnectorCallInterfacesMatchTheFrozenSurface` | lifecycle, send, request와 wait call의 정확한 member를 고정한다. |
| `StreamConnectorTests.ConnectorOptionsMatchTheFrozenDefaults` | connector option의 기본값을 고정한다. |
| `StreamConnectorTests.ManualDispatchRunsHandlerOnDispatchCaller` | Manual callback은 dispatch caller에서 실행된다. |
| `StreamConnectorTests.ImmediateDispatchRunsHandlerWithoutManualDispatch` | Immediate callback은 별도 manual dispatch 없이 실행된다. |
| `StreamConnectorTests.ManualRequestCallbackAdmission_Is_Bounded_And_Never_Falls_Back_To_A_Background_Thread` | request callback admission은 bounded이며 background 우회를 허용하지 않는다. |
| `StreamConnectorTests.RequestTimeoutRemovesPendingRequest` | timeout 뒤 pending request를 제거한다. |
| `StreamConnectorTests.TcpTypedRequestCorrelatesResponse` | typed request와 response correlation을 유지한다. |
| `StreamConnectorTests.TypedConnectorUsesJsonByDefaultAndDecodeReply` | typed 기본 codec은 JSON이다. |
| `StreamConnectorTests.PacketNameAttributeIsUsedByDefault` | [packet name](../../../01-glossary.ko.md#packet-name) attribute를 기본 identity로 사용한다. |
| `StreamConnectorTests.DisconnectEventCarriesTheFrozenCloseReasonContract` | disconnect event의 닫힌 종료 사유를 고정한다. |
| `StreamConnectorTests.SessionClosingPublishesServerDrainReasonAfterDisconnectedState` | session-closing frame을 `ServerDrain` 사유로 변환한다. |
| `StreamConnectorTests.SharedCloseFaultIsObservedByRepeatedCloseAndDispose` | 반복 close와 dispose가 같은 실패를 관찰한다. |
| `StreamConnectorTests.OneWayAsync_Waits_For_Bounded_Queue_Admission` | one-way terminal은 bounded queue 수락까지 비동기로 기다리고 결과값 없이 완료한다. |
| `StreamConnectorTests.RequestQueueWaitsForEarlierAcceptedOneWaySend` | 먼저 수락된 one-way send와 뒤 request의 wire 전송 순서를 보존한다. |
| `StreamConnectorTests.CallerCancellationDoesNotInterruptAnInProgressFrameWrite` | frame write가 시작된 뒤에는 caller cancellation이 partial frame을 만들지 않는다. |
| `StreamConnectorTests.InboundObserverRegistrationIsRejectedAfterConnectAndStopsAfterDispose` | observer 등록 시점과 해제 의미를 고정한다. |
| `StreamConnectorTests.Dispose_Waits_For_Cancellation_Ignoring_Inbound_Observer` | dispose는 cancellation을 무시하는 observer 종료를 기다린다. |
| `StreamConnectorTests.InboundObserverFailureReportsObserverFailedAndMessageStillDispatches` | observer 실패를 보고하면서 원래 message를 계속 처리한다. |
| `StreamConnectorTests.InboundObserverOverflowReportsObserverDroppedAndRequestStillCompletes` | observer overflow가 request 완료를 막지 않는다. |
| `StreamConnectorTests.OutboundFrameCreatesFlowOnceAndCodecRemainsDeterministic` | outbound flow를 한 번 생성하고 header codec 결과를 고정한다. |
| `StreamConnectorTests.HeaderProtocolEnforcesControlPacketContract` | control packet의 codec·flag·payload 계약을 고정한다. |

Release 검증은 `scripts/verify_packaged_contract.sh`로 source assembly, API snapshot, 실제 NuGet
package와 clean consumer가 모두 같은 공개 계약인지 확인한다.

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../../../../README.ko.md)
<!-- framework-adapter-nav:bottom:end -->
