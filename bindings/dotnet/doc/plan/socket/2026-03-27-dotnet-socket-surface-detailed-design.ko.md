# .NET Socket Surface 상세 설계

작성일: 2026-03-27

## 1. 목적

이 문서는 `bindings/dotnet`의 raw socket 계층 public surface를 재정의한다.

목표는 두 가지다.

- `.NET` 사용자가 socket 타입별로 허용된 동작을 API shape만 보고 이해하게 만들 것
- interop, lifecycle, callback, option 공통 메커니즘은 하나의 깊은 내부 모듈로 모아 change amplification을 줄일 것

즉, 클래스는 타입별로 분리하되 구현을 여기저기 복제하지 않는다. public surface는 제한하고, 실제 복잡성은 internal kernel에 숨긴다.

## 2. 현재 문제 정의

현재 [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs) 는 다음 책임을 한 클래스에 동시에 담고 있다.

- bind/connect/unbind/disconnect lifecycle
- raw message `Send` / `Receive`
- topic `Publish` / `Subscribe`
- subscription event receive
- recv callback / subscribe callback / send-ready callback
- STREAM raw callback
- common option / type-specific option get/set
- monitor open
- discovery attach
- routing id / TLS / typed option marshalling helper

이 구조의 문제는 명확하다.

- `PUB` 사용자가 `Receive(...)`, `Subscribe(...)`, `AttachStreamRaw(...)` 같은 unrelated API를 전부 보게 된다.
- `STREAM` 전용 의미와 `XPUB` 전용 의미가 generic `Socket`에 섞인다.
- unsupported 동작을 runtime 예외에 의존하게 되어 surface가 shallow해진다.
- public class를 이해하려면 native socket type 규칙을 사용자가 직접 외워야 한다.

POSD 관점에서 이것은 "하나의 큰 public surface + 내부 분기문" 구조다. 구현은 한 파일에 모였지만 interface가 얕고 넓어서 사용자가 너무 많은 것을 알아야 한다.

## 3. 설계 원칙

- socket 타입별 클래스는 "새 구현체"가 아니라 "제한된 facade"다.
- lifecycle, interop, callback state, common option marshalling은 internal 깊은 모듈 하나에 모은다.
- public API에서는 raw message 계층과 topic 계층을 분리한다.
- `Send` / `Receive` 와 `Publish` / `Subscribe` 를 같은 클래스에 섞지 않는다.
- `Message` 는 payload container이며 bytes/string convenience 책임도 계속 `Message` 가 가진다.
- public API는 data-plane과 special API에서 compile-time surface 제한을 우선하고, unsupported runtime 예외를 줄인다.
- 기존 `SocketOptionKey<T>` 체계는 이번 refactor에서 유지한다. option taxonomy 재설계는 별도 작업으로 분리한다.
- `.NET` 사용자는 `SocketType` enum 기반 generic constructor보다 concrete class를 기본 진입점으로 사용해야 한다.
- hot path에서 추가 allocation이나 delegate trampoline 증가를 만들지 않는다.
- 이 refactor에서 `Spot`, `Discovery`, `Registry` 는 직접 분해하지 않는다. raw socket 계층만 다룬다.

## 4. native 기준 확정 범위

이번 설계가 전제로 삼는 raw socket type은 최신 `core` 의 아래 8종뿐이다.

- `Pair`
- `Pub`
- `Sub`
- `Dealer`
- `Router`
- `XPub`
- `XSub`
- `Stream`

이번 설계에서 제외하는 타입:

- `Push`
- `Pull`
- `Scatter`
- `Gather`
- `Req`
- `Rep`

제외 이유:

- 최신 [`zlink.h`](/home/hep7/project/kairos/zlink/core/include/zlink.h) 기준 raw socket type으로 존재하지 않는다.
- `.NET` 에서만 facade로 부활시키면 shallow wrapper와 문서 부채만 늘어난다.

## 5. 최종 계층 구조

```text
internal SocketHandle
  ^
  |
internal SocketKernel
  ^
  |
public abstract SocketBase
  ^
  +-- public abstract MessageSocketBase
  |     +-- public sealed PairSocket
  |     +-- public sealed DealerSocket
  |     +-- public sealed RouterSocket
  |     +-- public sealed StreamSocket
  |
  +-- public abstract PublisherSocketBase
  |     +-- public sealed PubSocket
  |     +-- public sealed XPubSocket
  |
  +-- public abstract SubscriberSocketBase
        +-- public sealed SubSocket
        +-- public sealed XSubSocket

compat:
  public sealed Socket   // phase 1 유지, phase 2 obsolete
```

정책:

- `SocketHandle` 은 native handle ownership utility다. public으로 노출하지 않는다.
- `SocketKernel` 은 진짜 깊은 모듈이다. interop와 공통 정책을 가진다.
- `SocketBase` 는 lifecycle과 공통 역할만 노출한다.
- `MessageSocketBase`, `PublisherSocketBase`, `SubscriberSocketBase` 는 의미별 facade다.
- concrete type은 public surface 제한과 special API 노출만 담당한다.

## 6. 파일 배치

최종 파일 배치는 아래로 고정한다.

- `src/Zlink/Sockets/Internal/SocketHandle.cs`
- `src/Zlink/Sockets/Internal/SocketKernel.cs`
- `src/Zlink/Sockets/SocketBase.cs`
- `src/Zlink/Sockets/MessageSocketBase.cs`
- `src/Zlink/Sockets/PublisherSocketBase.cs`
- `src/Zlink/Sockets/SubscriberSocketBase.cs`
- `src/Zlink/Sockets/PairSocket.cs`
- `src/Zlink/Sockets/DealerSocket.cs`
- `src/Zlink/Sockets/RouterSocket.cs`
- `src/Zlink/Sockets/StreamSocket.cs`
- `src/Zlink/Sockets/PubSocket.cs`
- `src/Zlink/Sockets/XPubSocket.cs`
- `src/Zlink/Sockets/SubSocket.cs`
- `src/Zlink/Sockets/XSubSocket.cs`
- `src/Zlink/Socket.cs`

배치 원칙:

- 새 public 타입은 canonical root인 `namespace Systems.Zlink;` 에 둔다.
  `namespace Systems.Zlink.Sockets;` 같은 새 public namespace는 만들지 않는다.
- internal 타입은 파일 경로만 `Sockets/Internal` 로 나누고 namespace는
  `Systems.Zlink.Sockets.Internal` 로 둔다.
- `Socket.cs` 는 새 구조 도입 후 generic compat shim으로 축소한다.
- 새 샘플과 새 contract test는 `new Socket(ctx, SocketType.X)` 를 사용하지 않는다.
- `NativeMethods.*` 와 `Message.cs` 는 이번 refactor의 dependency이지만 분리 대상은 아니다.

## 7. 내부 구조 결정

### 7.0 구현 결정 고정

이번 문서에서 더 이상 열어두지 않는 결정은 아래다.

- interop ownership과 native lifecycle은 composition 기반이다.
- public facade는 제한된 계층 상속을 사용한다.
  - `SocketBase`
  - `MessageSocketBase` / `PublisherSocketBase` / `SubscriberSocketBase`
  - concrete socket
- `Socket` compat는 typed socket을 상속하지 않는다.
- `SocketKernel` 은 `internal sealed class` 로 고정한다.
- `SocketBase` 는 `readonly SocketKernel _kernel;` 를 가진 abstract facade다.
- concrete socket은 public 생성자 하나만 둔다.
  - 예: `public PairSocket(Context context)`
- concrete socket의 internal adoption constructor는 두지 않는다. adoption/borrow는 kernel 내부 책임으로 한정한다.
- callback delegate field와 native delegate pinning state는 전부 `SocketKernel` 이 가진다.
- `Socket` compat class는 phase 1에서는 유지하되, 구현은 거의 전부 kernel 위임으로 축소한다.
- public delegate type은 새로 만들지 않는다.
  - `SocketRecvHandler`
  - `SocketSubscribeHandler`
  - `StreamPacketHandler`

### 7.1 `SocketHandle`

역할:

- raw native socket handle 소유
- own / borrow 구분
- `Close()` 와 `DangerousGetHandle()` 수준의 최소 API만 제공

고정 정책:

- `SafeHandle` 로 바꾸지 않는다.
- 이유는 이 refactor의 목적이 public surface 축소와 책임 분리이지 low-level lifetime model 교체가 아니기 때문이다.
- hot path P/Invoke가 모두 handle lookup 비용을 추가로 지게 만드는 변경은 측정 없이 넣지 않는다.

### 7.2 `SocketKernel`

역할:

- bind/connect/unbind/disconnect
- discovery attach
- common option marshalling
- router/dealer/pub/sub/stream option domain dispatch
- monitor open
- recv/subscribe/send-ready callback state 보관
- STREAM raw callback state 보관
- `Send`, `Receive`, `Publish`, `Subscribe`, `ReceiveSubscriptionEvent`의 공통 interop 구현

핵심 원칙:

- public facade는 thin wrapper지만, `SocketKernel` 은 thin wrapper가 아니다.
- callback ownership, send ownership, routing id codec, topic buffer 정책 같은 계약은 여기에 모은다.
- `SocketKernel` 은 concrete socket type 제약을 모른다. 의미 제약은 facade가 담당한다.

고정 shape:

```csharp
internal sealed class SocketKernel : IDisposable
{
    internal SocketKernel(Context context, SocketType type);

    internal void Bind(string endpoint);
    internal void Connect(string endpoint);
    internal void Unbind(string endpoint);
    internal void Disconnect(string endpoint);

    internal void AttachDiscovery(Discovery discovery);
    internal void SendReadyHandler(Action handler);
    internal SocketMonitor OpenMonitor(SocketEvent events);

    internal void Send(Message message, SendFlags flags);
    internal void Send(IReadOnlyList<Message> parts, SendFlags flags);
    internal void Send(string routingId, Message message, SendFlags flags);
    internal void Send(string routingId, IReadOnlyList<Message> parts, SendFlags flags);

    internal void Publish(string topic, Message message, SendFlags flags);
    internal void Publish(string topic, IReadOnlyList<Message> parts, SendFlags flags);

    internal void Receive(out Message message, ReceiveFlags flags);
    internal void Receive(out Message[] parts, ReceiveFlags flags);
    internal void Receive(out string routingId, out Message message, ReceiveFlags flags);
    internal void Receive(out string routingId, out Message[] parts, ReceiveFlags flags);

    internal void Subscribe(out string topic, out Message message, ReceiveFlags flags);
    internal void Subscribe(out string topic, out Message[] parts, ReceiveFlags flags);
    internal void Subscribe(out string routingId, out string topic, out Message message, ReceiveFlags flags);
    internal void Subscribe(out string routingId, out string topic, out Message[] parts, ReceiveFlags flags);
    internal void SubscribeHandler(SocketSubscribeHandler handler);

    internal void ReceiveSubscriptionEvent(out string topic, out bool subscribed, ReceiveFlags flags);
    internal void ReceiveSubscriptionEvent(out string routingId, out string topic, out bool subscribed, ReceiveFlags flags);

    internal void RecvHandler(SocketRecvHandler handler);
    internal void AttachStreamRaw(StreamPacketHandler handler);
    internal void DetachStream();
}
```

중요:

- `SocketKernel` 은 모든 역할을 갖지만 public surface는 facade가 제한한다.
- typed facade는 kernel 메서드 중 자신에게 의미 있는 것만 노출한다.
- ownership 계약은 현재 구현과 동일해야 하며, 이번 refactor에서 바꾸지 않는다.

## 8. public 클래스별 책임

### 8.1 `SocketBase`

역할:

- `IDisposable`
- `Bind`, `Connect`, `Unbind`, `Disconnect`
- `AttachDiscovery`
- `SendReadyHandler`
- `OpenMonitor`
- common option get/set

고정 인터페이스:

```csharp
public abstract class SocketBase : IDisposable
{
    protected SocketBase(SocketKernel kernel);

    public void Bind(string endpoint);
    public void Connect(string endpoint);
    public void Unbind(string endpoint);
    public void Disconnect(string endpoint);

    public void AttachDiscovery(Discovery discovery);
    public void SendReadyHandler(Action handler);
    public SocketMonitor OpenMonitor(SocketEvent events);

    public void SetOption(SocketOptionKey<int> option, int value);
    public void SetOption(SocketOptionKey<long> option, long value);
    public void SetOption(SocketOptionKey<ulong> option, ulong value);
    public void SetOption(SocketOptionKey<byte[]> option, ReadOnlySpan<byte> value);
    public void SetOption(SocketOptionKey<string> option, string value);

    public int GetOption(SocketOptionKey<int> option);
    public long GetOption(SocketOptionKey<long> option);
    public ulong GetOption(SocketOptionKey<ulong> option);
    public byte[] GetOption(SocketOptionKey<byte[]> option, int initialSize = 256);
    public int GetOption(SocketOptionKey<byte[]> option, Span<byte> destination);
    public string GetOption(SocketOptionKey<string> option, int initialSize = 256);
}
```

제약:

- `SocketBase` 에는 `Send`, `Receive`, `Publish`, `Subscribe` 를 public으로 두지 않는다.
- data-plane 메서드는 하위 facade가 의미에 맞게 노출한다.

### 8.2 `MessageSocketBase`

역할:

- raw message transport facade
- `Send` / `Receive` / `RecvHandler`

대상 타입:

- `Pair`
- `Dealer`
- `Router`
- `Stream`

고정 인터페이스:

```csharp
public abstract class MessageSocketBase : SocketBase
{
    public void Send(Message message, SendFlags flags = SendFlags.None);
    public void Send(IReadOnlyList<Message> parts, SendFlags flags = SendFlags.None);

    public void Send(string routingId, Message message,
        SendFlags flags = SendFlags.None);
    public void Send(string routingId, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None);

    public void Receive(out Message message,
        ReceiveFlags flags = ReceiveFlags.None);
    public void Receive(out Message[] parts,
        ReceiveFlags flags = ReceiveFlags.None);

    public void Receive(out string routingId, out Message message,
        ReceiveFlags flags = ReceiveFlags.None);
    public void Receive(out string routingId, out Message[] parts,
        ReceiveFlags flags = ReceiveFlags.None);

    public void RecvHandler(SocketRecvHandler handler);
}
```

정책:

- method 이름은 `Send` / `Receive` 로 통일한다.
- single-part / multipart / routed variation은 overload로만 구분한다.
- ownership 계약은 현재와 동일하게 유지한다.
  - send 성공 시 library가 ownership 소비
  - recv/callback 수신 시 앱이 ownership 소비
- `AttachStreamRaw` 는 여기 두지 않는다. `StreamSocket` concrete type에만 둔다.

### 8.3 `PublisherSocketBase`

역할:

- topic publish facade

대상 타입:

- `Pub`
- `XPub`

고정 인터페이스:

```csharp
public abstract class PublisherSocketBase : SocketBase
{
    public void Publish(string topic, Message message,
        SendFlags flags = SendFlags.None);
    public void Publish(string topic, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None);
}
```

### 8.4 `SubscriberSocketBase`

역할:

- topic subscription 관리
- topic recv / subscribe callback

대상 타입:

- `Sub`
- `XSub`

고정 인터페이스:

```csharp
public abstract class SubscriberSocketBase : SocketBase
{
    public void SetSubscription(string topicOrPattern);
    public void UnsetSubscription(string topicOrPattern);

    public void Subscribe(out string topic, out Message message,
        ReceiveFlags flags = ReceiveFlags.None);
    public void Subscribe(out string topic, out Message[] parts,
        ReceiveFlags flags = ReceiveFlags.None);

    public void Subscribe(out string routingId, out string topic, out Message message,
        ReceiveFlags flags = ReceiveFlags.None);
    public void Subscribe(out string routingId, out string topic, out Message[] parts,
        ReceiveFlags flags = ReceiveFlags.None);

    public void SubscribeHandler(SocketSubscribeHandler handler);
}
```

## 9. concrete 타입 facade 정의

### 9.1 message 계열

- `PairSocket : MessageSocketBase`
- `DealerSocket : MessageSocketBase`
- `RouterSocket : MessageSocketBase`
- `StreamSocket : MessageSocketBase`

추가 정책:

- `StreamSocket` 에만 `AttachStreamRaw(StreamPacketHandler)` / `DetachStream()` 를 둔다.
- `PairSocket` 에는 routed overload를 숨기지 않는다. native surface와 맞는 raw message 계층으로 유지한다.
- `DealerSocket`, `RouterSocket`, `StreamSocket` 의 routing id overload는 그대로 유지한다.
- `PairSocket` 도 `MessageSocketBase` 공통 API를 그대로 받는다. 별도 축소 facade는 이번 refactor에 넣지 않는다.

### 9.2 pub/sub 계열

- `PubSocket : PublisherSocketBase`
- `XPubSocket : PublisherSocketBase`
- `SubSocket : SubscriberSocketBase`
- `XSubSocket : SubscriberSocketBase`

추가 정책:

- `XPubSocket` 에만 `ReceiveSubscriptionEvent(...)` 를 둔다.
- `PubSocket` / `XPubSocket` 에는 `Subscribe(...)` 를 두지 않는다.
- `SubSocket` / `XSubSocket` 에는 `Publish(...)` 를 두지 않는다.

구체 생성자 shape:

```csharp
public sealed class PairSocket : MessageSocketBase
{
    public PairSocket(Context context);
}

public sealed class DealerSocket : MessageSocketBase
{
    public DealerSocket(Context context);
}

public sealed class RouterSocket : MessageSocketBase
{
    public RouterSocket(Context context);
}

public sealed class StreamSocket : MessageSocketBase
{
    public StreamSocket(Context context);
    public void AttachStreamRaw(StreamPacketHandler handler);
    public void DetachStream();
}

public sealed class PubSocket : PublisherSocketBase
{
    public PubSocket(Context context);
}

public sealed class XPubSocket : PublisherSocketBase
{
    public XPubSocket(Context context);
    public void ReceiveSubscriptionEvent(out string topic, out bool subscribed,
        ReceiveFlags flags = ReceiveFlags.None);
    public void ReceiveSubscriptionEvent(out string routingId, out string topic,
        out bool subscribed, ReceiveFlags flags = ReceiveFlags.None);
}

public sealed class SubSocket : SubscriberSocketBase
{
    public SubSocket(Context context);
}

public sealed class XSubSocket : SubscriberSocketBase
{
    public XSubSocket(Context context);
}
```

## 10. 허용 인터페이스 매트릭스

| 클래스 | bind/connect | send/receive | publish/subscribe | option surface | special |
|---|---|---|---|---|---|
| `PairSocket` | O | `Send` / `Receive` | X | common `SetOption` / `GetOption` | 없음 |
| `DealerSocket` | O | `Send` / `Receive` | X | common `SetOption` / `GetOption` | 없음 |
| `RouterSocket` | O | `Send` / `Receive` | X | common `SetOption` / `GetOption` | 없음 |
| `StreamSocket` | O | `Send` / `Receive` | X | common `SetOption` / `GetOption` | `AttachStreamRaw` |
| `PubSocket` | O | X | `Publish` | common `SetOption` / `GetOption` | 없음 |
| `XPubSocket` | O | X | `Publish` | common `SetOption` / `GetOption` | `ReceiveSubscriptionEvent` |
| `SubSocket` | O | topic `Subscribe` | `Set/UnsetSubscription` | common `SetOption` / `GetOption` | 없음 |
| `XSubSocket` | O | topic `Subscribe` | `Set/UnsetSubscription` | common `SetOption` / `GetOption` | 없음 |

의미:

- `PubSocket` 에는 `Receive(...)` 가 없다.
- `SubSocket` 에는 `Send(...)` 와 `Publish(...)` 가 없다.
- `StreamSocket` 특수 API는 `StreamSocket` 에서만 보인다.
- `XPubSocket` 의 subscription event는 generic publisher API에 넣지 않는다.

## 11. option 노출 규칙

이 항목은 구현 가능성을 기준으로 다시 고정한다.

- 이번 refactor에서는 option key taxonomy를 분리하지 않는다.
- 즉, `DealerOptionKey<T>`, `RouterOptionKey<T>` 같은 새 public 타입은 만들지 않는다.
- 따라서 option의 compile-time 제한은 이번 작업 목표가 아니다.
- 이번 refactor의 1차 목표는 data-plane / special API의 surface 분리다.

### 11.1 공통 option

`SocketBase` 에 둔다.

예:

- affinity
- backlog
- linger
- sndhwm / rcvhwm
- sndbuf / rcvbuf
- sndtimeo / rcvtimeo
- reconnect interval
- heartbeat 계열
- routing id option
- TLS option

### 11.2 타입별 option 정책

이번 refactor에서의 고정 결정:

- 기존 global [`SocketOptions.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/SocketOptions.cs) 는 유지한다.
- `SetOption` / `GetOption` 은 `SocketBase` 에 그대로 둔다.
- concrete socket별 option compile-time 제한은 이번 작업 범위에서 제외한다.
- 대신 아래 special API만 concrete type으로 분리한다.
  - `StreamSocket.AttachStreamRaw`
  - `XPubSocket.ReceiveSubscriptionEvent`
  - `SubscriberSocketBase.SetSubscription`
  - `SubscriberSocketBase.Subscribe`
  - `PublisherSocketBase.Publish`

이유:

- 현재 `SocketOptionKey<T>` 모델은 key 자체가 socket domain 정보를 타입으로 갖지 않는다.
- 이 상태에서 option을 compile-time 제한하려면 key model까지 다시 설계해야 한다.
- 그건 socket class 분리보다 더 큰 public API 변경이므로 별도 refactor로 분리하는 것이 POSD 관점에서 맞다.

## 12. `.NET` 스타일 규칙

- public 클래스 이름은 `PairSocket`, `RouterSocket` 처럼 PascalCase concrete type을 사용한다.
- public API에 `IntPtr`, native struct, raw handle은 노출하지 않는다.
- `SocketType` enum은 public compat 요소로 유지하되, 새 코드의 기본 진입점은 concrete class 생성자다.
- hot path는 `IEnumerable<T>` 대신 `IReadOnlyList<Message>` 와 direct overload를 유지한다.
- extension method로 socket type 차이를 흉내 내지 않는다.
- marker interface(`IPublisherSocket`, `ISubscriberSocket`) 는 이번 refactor에 넣지 않는다.
- public namespace는 계속 `Zlink` 다.
- public abstract base는 최소 3단계까지만 둔다.
  - `SocketBase`
  - `MessageSocketBase` / `PublisherSocketBase` / `SubscriberSocketBase`
  - concrete sealed type

## 13. anti-goals

- `Socket.cs` 를 partial 파일 여러 개로 쪼개는 것으로 refactor를 끝내지 않는다.
- generic `Socket` 에서 runtime 체크만 더 넣는 방식으로 surface를 유지하지 않는다.
- `SocketType` switch를 public facade 곳곳에 복제하지 않는다.
- type-specific option을 다시 하나의 giant `SocketOptions` public matrix로 되돌리지 않는다.
- `Spot` 을 raw socket hierarchy에 억지로 끼워 넣지 않는다.

## 14. compatibility 정책

### 14.1 phase 1

- 기존 `Socket` public class는 유지한다.
- 내부 구현은 `SocketKernel` 로 위임한다.
- 새 typed socket과 동일한 kernel을 사용한다.
- `Socket` 은 상속 기반 compat가 아니라 composition 기반 compat다.
- `Socket` 은 generic 역할을 그대로 유지하지만, 새 샘플과 새 문서에서는 사용하지 않는다.

### 14.2 phase 2

- `Socket(Context, SocketType)` 생성자에 `[Obsolete]` 를 검토한다.
- 새 샘플과 새 문서는 concrete socket만 사용한다.

### 14.3 phase 3

- major version에서 generic `Socket` 제거 여부를 결정한다.
- 제거 전까지는 compat shim 외 역할을 갖지 않게 축소한다.

### 14.4 old-to-new 매핑

새 문서와 새 샘플은 아래 매핑을 기준으로 작성한다.

| 기존 | 새 기준 |
|---|---|
| `new Socket(ctx, SocketType.Pair)` | `new PairSocket(ctx)` |
| `new Socket(ctx, SocketType.Dealer)` | `new DealerSocket(ctx)` |
| `new Socket(ctx, SocketType.Router)` | `new RouterSocket(ctx)` |
| `new Socket(ctx, SocketType.Stream)` | `new StreamSocket(ctx)` |
| `new Socket(ctx, SocketType.Pub)` | `new PubSocket(ctx)` |
| `new Socket(ctx, SocketType.XPub)` | `new XPubSocket(ctx)` |
| `new Socket(ctx, SocketType.Sub)` | `new SubSocket(ctx)` |
| `new Socket(ctx, SocketType.XSub)` | `new XSubSocket(ctx)` |

compat 정책:

- 기존 호출 코드는 phase 1에서 동작 유지
- 새 코드 작성 기준만 concrete socket으로 바꿈
- compat 제거는 이번 작업의 완료 조건이 아님

## 15. 구현 순서

### 15.0 파일별 이동 맵

현재 [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs) 에서 이동할 범위를 먼저 고정한다.

- `Bind`, `Connect`, `Unbind`, `Disconnect`
  - `SocketKernel`
- `AttachDiscovery`, `SendReadyHandler`, `OpenMonitor`
  - `SocketKernel`
- `Send*`, `Receive*`, `Publish*`, `Subscribe*`, `ReceiveSubscriptionEvent*`
  - `SocketKernel`
- `RecvHandler`, `SubscribeHandler`, `AttachStreamRaw`, `DetachStream`
  - `SocketKernel`
- callback native trampoline과 delegate field
  - `SocketKernel`
- option get/set helper와 routing id / TLS helper
  - `SocketKernel`
- public method forwarding
  - `SocketBase` / derived facade / compat `Socket`

이번 작업에서 유지할 파일:

- [`Message.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Message.cs)
- [`RoutingIdCodec.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/RoutingIdCodec.cs)
- [`SocketOptions.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/SocketOptions.cs)
- [`Monitor.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Monitor.cs)

phase별 실제 수정 파일 고정:

- phase 1
  - 추가: `Sockets/Internal/SocketHandle.cs`
  - 추가: `Sockets/Internal/SocketKernel.cs`
  - 수정: [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs)
- phase 2
  - 추가: `Sockets/SocketBase.cs`
  - 추가: `Sockets/MessageSocketBase.cs`
  - 추가: `Sockets/PublisherSocketBase.cs`
  - 추가: `Sockets/SubscriberSocketBase.cs`
- phase 3
  - 추가: `Sockets/PairSocket.cs`
  - 추가: `Sockets/DealerSocket.cs`
  - 추가: `Sockets/RouterSocket.cs`
  - 추가: `Sockets/StreamSocket.cs`
  - 추가: `Sockets/PubSocket.cs`
  - 추가: `Sockets/XPubSocket.cs`
  - 추가: `Sockets/SubSocket.cs`
  - 추가: `Sockets/XSubSocket.cs`
- phase 4
  - 수정: [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs)
  - 수정: typed concrete socket 파일
- phase 5
  - 수정: tests
  - 수정: docs
  - 수정: samples
- phase 6
  - 수정: `src/Zlink/Sockets/*.cs`
  - 수정: [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs)
  - 수정: 관련 tests / docs

### phase 1. internal kernel 추출

- `SocketHandle`
- `SocketKernel`
- 현재 `Socket.cs` 에서 handle/callback/interop/helper 로직 이동

완료 기준:

- public API 변화 없이 기존 테스트 통과
- `Socket.cs` 는 facade 수준으로 얇아짐
- `Socket.cs` 내부에 native callback trampoline 구현이 남아 있지 않음
- `Socket.cs` 내부에 option marshalling helper가 남아 있지 않음

### phase 2. abstract facade 도입

- `SocketBase`
- `MessageSocketBase`
- `PublisherSocketBase`
- `SubscriberSocketBase`

완료 기준:

- 공통 public API가 base로 이동
- 하위 facade가 semantic surface를 분리
- base class는 전부 `abstract`
- concrete 구현 없는 public helper static class를 새로 만들지 않음

### phase 3. concrete socket 도입

- `PairSocket`
- `DealerSocket`
- `RouterSocket`
- `StreamSocket`
- `PubSocket`
- `XPubSocket`
- `SubSocket`
- `XSubSocket`

완료 기준:

- concrete class 생성자만으로 해당 socket 사용 가능
- type별로 irrelevant method가 public surface에서 사라짐
- 새 concrete class는 모두 `sealed`
- 새 public namespace 추가 없음

### phase 4. special API 분리

- `XPubSocket.ReceiveSubscriptionEvent`
- `StreamSocket.AttachStreamRaw`
- compat `Socket` forwarding 정리

완료 기준:

- special API가 올바른 concrete 타입에만 존재
- option API는 여전히 `SocketBase` 에 공통으로 존재

### phase 5. compat 및 문서/샘플 정리

- `Socket` compat shim 축소
- 샘플과 contract test를 typed socket으로 전환
- 문서 업데이트

완료 기준:

- 새 사용자 경로는 concrete socket만 사용
- compat는 legacy path로만 남음

### phase 6. POSD 기반 잔여 리팩토링

- facade/kernel 경계의 중복과 hidden coupling 제거
- 설명하기 어려운 lifecycle/callback invariant 단순화
- 남은 shallow wrapper/중복 forwarding/임시 어댑터 정리
- complexity reduction 근거를 남기며 반복 수행

완료 기준:

- 설명 가능한 잔여 리팩토링 대상이 더 이상 남지 않음
- 남은 복잡성이 intentional tradeoff로 정리 가능
- 추가 구조 변경이 complexity 감소보다 비용이 크다고 판단 가능

## 16. 테스트 전략

이 refactor에서 필요한 테스트는 core 재검증이 아니라 surface 계약 검증이다.

남겨야 할 테스트:

- 각 concrete socket의 public method set이 기대 shape인지 reflection으로 검증하는 API surface test
- send ownership / recv ownership / callback ownership contract
- `XPubSocket.ReceiveSubscriptionEvent` 계약
- `StreamSocket.AttachStreamRaw` 계약
- compat `Socket` 과 typed socket이 같은 동작 계약을 유지하는지 검증

추가할 테스트 파일 예시:

- `test_pair_socket_surface.cs`
- `test_router_socket_surface.cs`
- `test_stream_socket_surface.cs`
- `test_pubsub_socket_surface.cs`
- `test_xpub_socket_surface.cs`
- `test_socket_compat_layer.cs`

검증 방식 고정:

- negative compile test 프로젝트는 이번 refactor에 넣지 않는다.
- 대신 reflection으로 public instance method 이름/시그니처를 snapshot처럼 검증한다.
- 사용 예제 검증은 samples가 아니라 기존 contract test와 새 surface test로 한다.

삭제/축소 대상:

- generic `Socket` 하나로 모든 타입을 훑는 surface 테스트
- irrelevant API가 runtime 예외를 던지는지만 보는 테스트

## 17. PR 분할 권장

1. internal kernel 추출
2. abstract facade 도입
3. concrete socket 추가
4. compat shim 축소 + 샘플/테스트 전환

한 PR에 public shape 전체를 뒤엎지 않는다. 각 단계마다 build/test 가능한 slice로 나눈다.

## 18. 구현 착수 체크리스트

- `namespace Systems.Zlink` 유지
- `SocketKernel` 은 `internal sealed`
- `SocketBase` 는 composition, inheritance 아님
- `Socket` compat는 composition 유지
- option key taxonomy 변경 안 함
- `Message` ownership 계약 변경 안 함
- `StreamSocket.AttachStreamRaw` 는 concrete 타입에만 노출
- `XPubSocket.ReceiveSubscriptionEvent` 는 concrete 타입에만 노출
- 새 public 클래스는 모두 `sealed`
- 새 샘플/문서는 generic `Socket` 사용 안 함
- `dotnet build` / `dotnet test` 로 각 phase 종료 시 검증
- reflection 기반 surface test 추가
- old-to-new 매핑표 기준으로 샘플 경로 전환
- 마지막에 POSD 기준 잔여 리팩토링 검토 수행

## 19. API review gate

구현 후 아래 질문에 모두 `예` 라고 답할 수 있어야 한다.

- `PubSocket` 사용자가 `Receive(...)` 와 `Subscribe(...)` 를 보지 않는가?
- `SubSocket` 사용자가 `Publish(...)` 를 보지 않는가?
- `StreamSocket` 특수 API가 다른 socket에서 사라졌는가?
- generic `Socket` 이 실제 구현체가 아니라 compat facade로 축소되었는가?
- callback/ownership/send 계약이 refactor 전과 동일한가?
- option taxonomy를 억지로 같이 바꾸지 않았는가?
- 사용자가 알아야 할 native socket rule 수가 이전보다 줄었는가?
- 더 진행할 가치가 있는 POSD 리팩토링 target이 실제로 남아 있는가?

## 20. 최종 결정

이번 refactor의 핵심 결정은 아래다.

- `.NET` raw socket의 기본 진입점은 앞으로 concrete socket class다.
- public surface 분리는 class로 하고, interop 복잡성은 internal kernel 하나에 숨긴다.
- `Socket` generic class는 바로 지우지 않고 compat shim으로 축소한다.
- `Send` / `Receive`, `Publish` / `Subscribe` naming 체계는 유지하되 클래스 경계로 의미를 분리한다.
- POSD 기준으로 이 refactor의 성공은 "파일 수 증가"가 아니라 "사용자가 알아야 할 것 감소"로 판단한다.
- 구현 완료 후에도 마지막 phase에서 더 이상 complexity reduction target이 없을 때까지 POSD 기반 리팩토링을 반복한다.
