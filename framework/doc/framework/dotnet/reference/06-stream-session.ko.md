# 06. Stream session

[레퍼런스 목차](README.ko.md)

이 category는 STREAM session 코드 안에서 쓰는 진입점(`IZLinkSessionClient`, `IZLinkSessionActors`,
`IZLinkSessionActor`, `IZLinkStream`)과 Actor 코드 안에서 bound session에 쓰는 진입점
(`IZLinkBoundSession`)을 다룬다. 정확한 signature는
[STREAM session exact interface](../../common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md)와
[Bound STREAM session exact interface](../../common/spec/server/languages/dotnet/interfaces/07-bound-stream-session.ko.md)가
소유한다.

---

## Handler 등록 (Session 코드 안, `Configure()`)

이 STREAM session이 받을 packet handler type을 등록한다. `Context.Handlers`
(`IZLinkSessionHandlerRegistry`)로 호출하며, `Configure()` override 안에서만 호출한다.

```csharp
public void Configure()
{
    Context.Handlers.AddHandler<PingHandler>();
}
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.AddHandler<THandler>()` | packet name은 메시지 타입에서 결정 | `IZLinkSessionPacketHandler<TSessionContext, TMessage>` 구현체 등록 |
| `.AddHandler<THandler>(packetName)` | — | packet name을 명시적으로 지정 |

**완료 결과.** 반환값 없이 동기로 등록된다. `TryHandleAsync(dispatch, payload, ct)`는 등록된
handler로 실제 dispatch를 시도하는 내부 진입점이며, application 코드가 직접 호출하지 않는다 —
Framework의 recv loop가 수신한 각 packet마다 호출한다.

**선택 기준.** `Configure()`가 호출될 때마다 이 session이 처리할 모든 packet handler를 등록한다.

---

## `Send<TMessage>` (Session 코드 안)

연결된 client에 one-way message를 보낸다. `Context.Client.Send(...)`로 호출한다.

```csharp
await Context.Client
    .Send(new ServerTick(tickNumber))
    .Async(ct);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.Metadata(...)` | 없음 | client에 전달할 key-value |
| `.Compress()` | 비압축 | 등록된 stream compression codec으로 payload를 압축 |
| `.Async(ct)` | 필수 terminal | source-local admission까지만 기다린다 |

**완료 결과.** messaging-execution category의 one-way 완료 kind와 같다 — socket send
timeout까지 기다린 뒤 없으면 `DeadlineExceeded`, connection 단절은 `Unavailable`.

**선택 기준.** Client가 보낸 request가 아닌, server가 먼저 보내는 push 메시지에 쓴다. Client의
request에 답할 때는 `Reply`를 쓴다.

---

## `Reply<TMessage>` (Session 코드 안)

현재 처리 중인 request packet에 응답한다. `Context.Client.Reply(...)`로 호출한다.

```csharp
await Context.Client
    .Reply(new GetPlayerStateResult(state))
    .Async(ct);
```

**옵션.** 이 호출에는 `.Compress()`와 `.Async(ct)` terminal만 있다 — `Reply`는 metadata modifier가
없다.

**완료 결과.** 이 request의 one-shot reply token을 원자적으로 claim한 뒤 전송한다. 같은 token으로
만든 두 번째 `Reply` call은 claim에 실패해 transport를 시도하지 않고 exceptional completion으로
끝난다. Caller의 request timeout은 wire로 전달되지 않으므로 이 reply의 admission deadline은
STREAM socket send timeout만 사용한다. Timeout이나 cancellation 뒤에는 late reply를 보내지
않는다.

**선택 기준.** `ZLinkSessionDispatchContext.CanReply`가 true인 packet(request)에만 쓴다. Client가
보낸 것이 아닌 새 메시지를 보내려면 `Send`를 쓴다.

---

## `BindAsync` / `BindOrGetAsync` (Session↔Actor)

이 STREAM session에 Actor를 묶어 Actor 쪽에서 이 연결로 push할 수 있게 한다. `Context.Actors`로
호출한다.

```csharp
IZLinkSessionActor bound = await Context.Actors.BindOrGetAsync(actorRef, ct);
```

**옵션.** 이 호출에는 modifier가 없다 — `ActorRef`와 `CancellationToken`만 받는다.

**완료 결과.** `BindAsync`는 매번 새 binding을 만든다. `BindOrGetAsync`는 이미 bound된 같은
incarnation이 있으면 그것을 반환한다. Binding은 `ActorRef.ActorId + ObjectGeneration`의 exact
incarnation 하나로 고정된다. Mapping이 없으면 `NotFound`, generation이 다르면
`InvalidOperation`, pre-commit seal 중이면 `Unavailable`이다. `Find(actorId)`로 이미 bound된
handle을 동기 조회할 수 있다.

**선택 기준.** Actor가 이 client 연결로 직접 push해야 할 때 bind한다. Relocation이 일어나도
`IZLinkSessionActor.Ref`가 current location snapshot으로 갱신되므로 application이 다시 bind할
필요는 없다.

---

## `RelayAsync` / `NotifyDisconnectedAsync` (bound Actor handle)

Bind로 얻은 `IZLinkSessionActor`를 통해 이 Actor 쪽에서 client로 payload를 전달하거나 연결 단절을
통지한다.

```csharp
await bound.RelayAsync(ZLinkMessage.From(new RoomUpdated(state)), ct);
```

**옵션.** 두 호출 모두 modifier가 없다 — payload(`RelayAsync`)와 `CancellationToken`만 받는다.

**완료 결과.** `RelayAsync`는 source-local admission을 수락하면 정상 완료하는 one-way
operation이다. `NotifyDisconnectedAsync`는 connection이 유지된 상태에서 논리적 단절을 알리는
notification이며 callback terminal까지 기다린다. Physical disconnect는 Framework가 자동으로
현재 binding 전체에 통지하므로 이 호출이 그 대체 경로는 아니다.

**선택 기준.** Actor 쪽 코드에서 특정 bound client에 직접 전달할 때 쓴다. Request에 대한 응답은
Session 쪽 `Reply`가 처리한다.

---

## `Send<TMessage>` (Actor 코드 안, bound session)

Actor에서 자신에게 bind된 client로 one-way message를 보낸다. `Context.BoundSession.Send(...)`로
호출한다.

```csharp
await Context.BoundSession
    .Send(new InventoryChanged(item))
    .Async(ct);
```

**옵션.** `.Metadata(...)`와 필수 terminal `.Async(ct)`가 있다.

**완료 결과.** messaging-execution category의 one-way 완료 kind와 같다. 이 표면은 client를 향한
새 request operation을 제공하지 않는다 — client request에 대한 reply는 Actor request handler의
반환값으로 처리한다.

**선택 기준.** Actor 코드 쪽에서 bound client로 push할 때 쓴다. Session 쪽에서 직접 보내려면 위
`Send`(Session 코드 안) 항목을 쓴다. 연결을 끊으려면 `Context.BoundSession.DisconnectAsync(ct)`를
쓴다.

---

## `Write` (raw transport handle)

Typed call을 거치지 않고 STREAM transport에 직접 payload를 쓴다. Session callback의 `IZLinkStream`
handle에서 호출한다.

```csharp
bool written = stream.Write(ZLinkMessage.From(rawFrame), SendFlags.None);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `flags: SendFlags` | `SendFlags.None` | 저수준 전송 flag |

**완료 결과.** 동기 `bool`을 반환한다 — admission 성공 여부만 알려주며 typed call과 같은 예외
기반 완료 kind를 쓰지 않는다.

**선택 기준.** Typed `Send`/`Reply` call이 감당하지 못하는 저수준 전송이 필요할 때만 쓴다. 일반
업무 메시징에는 `Send`나 `Reply`를 쓴다.

---

## `CloseAsync` (연결 종료)

Session이나 raw transport handle을 닫는다. `IZLinkSessionContext.CloseAsync()`와
`IZLinkStream.CloseAsync()`가 각각 제공한다.

```csharp
await Context.CloseAsync();  // Session 쪽에서 이 연결을 닫는다
await stream.CloseAsync();   // transport handle에서 직접 닫는다
```

**옵션.** 두 호출 모두 modifier가 없다.

**완료 결과.** 연결을 닫는다. 이미 닫힌 연결에 다시 호출해도 이 문서가 정의하는 별도 예외 계약은
없다 — 정확한 재호출 의미는 exact interface를 확인한다.

**선택 기준.** Application이 자발적으로 이 STREAM 연결을 끊어야 할 때 쓴다. Actor 쪽에서 bound
client 연결을 끊으려면 `Context.BoundSession.DisconnectAsync(ct)`를 쓴다.

---

## `DisconnectAsync` (Actor 코드 안, bound session)

Actor에서 자신에게 bind된 client 연결을 끊는다. `Context.BoundSession.DisconnectAsync(ct)`로
호출한다.

```csharp
await Context.BoundSession.DisconnectAsync(ct);
```

**옵션.** 이 호출에는 modifier가 없다 — `CancellationToken`만 받는다.

**완료 결과.** Bound session과의 연결을 끊는다.

**선택 기준.** Actor 쪽 코드에서 특정 client 연결을 더 유지할 필요가 없을 때 쓴다. Session 쪽에서
직접 끊으려면 `CloseAsync` 항목을 쓴다.

---

전체 근거는
[STREAM session exact interface](../../common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md)와
[Bound STREAM session exact interface](../../common/spec/server/languages/dotnet/interfaces/07-bound-stream-session.ko.md)를
참고한다.
