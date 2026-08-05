한국어 | [English](02-messaging.en.md)

[레퍼런스 목차](README.ko.md)

# 02. Messaging

이 category는 message 소유권, receive envelope 타입(`Received`, `TopicMessage`,
`SubscriptionEvent`), 그리고 Sockets category의 모든 socket-type 진입점이 반환하는
send/request/reply operation-builder 공유 형태를 다룬다. 정확한 signature는
[`Contracts/Messaging/`](../../../../bindings/dotnet/src/Zlink/Contracts/Messaging/)이
소유한다. `Contracts/Messaging/MessageEnvelopeParts.cs`는 `internal`이라 public contract
항목이 없다.

---

## `Message`

zlink message payload 하나를 소유한다 — 모든 send·request·reply·receive API가 옮기는
단위다.

```csharp
using Message empty = new Message();
using Message sized = new Message(4096);
using Message copy = Message.From("payload"u8);
using Message fromString = Message.From("hello", Encoding.UTF8);
```

**옵션.**

| Member | 매개변수 | 의미 |
| --- | --- | --- |
| `Message()` | — | 빈 메시지 |
| `Message(int size)` | 음수는 `ArgumentOutOfRangeException` | 쓰기 가능 storage |
| `Message(ReadOnlySpan<byte>)` / `Message(ReadOnlyMemory<byte>)` | — | 스냅샷 복사 |
| `From(byte[])` / `From(ReadOnlySpan<byte>)` / `From(ReadOnlyMemory<byte>)` / `From(ReadOnlySequence<byte>)` | — | static factory, 스냅샷 복사 |
| `From(Message)` | — | 다른 메시지의 payload 복사 |
| `From(string)` / `From(string, Encoding)` | 기본 UTF-8 | 인코딩 |
| `Size` / `IsEmpty` / `RefCount` | — | 진단 |
| `AsSpan()` / `AsReadOnlySpan()` | — | 이 인스턴스 storage에 backing된 쓰기/읽기 전용 view |
| `AsReadOnlyMemory()` | — | native-backed message는 여기서 managed memory로 복사 |
| `ToArray()` / `GetString()` / `GetString(Encoding)` | — | 독립된 managed 복사본 |
| `CopyTo(Span<byte>)` / `CopyTo(IBufferWriter<byte>)` / `TryCopyTo(Span<byte>, out int)` | — | caller buffer로 복사 |

**완료 결과.** 모두 동기다. `Message`는 `IDisposable`/`IAsyncDisposable`이다 — 해제하면
payload storage가 반환된다. 이 메시지를 소비하는 submit(아래 Sockets/Messaging builder
형태)은 성공 후 managed instance를 빈 상태로 남긴다 — 그 후 payload를 읽으면 예외가
발생하지만, disposing은 여전히 안전하고 여전히 필요하다.

**선택 기준.** outbound payload를 만들 땐 크기 지정 또는 스냅샷 복사 생성자/factory를
쓴다. 추가 복사 없이 그대로 읽거나 쓸 땐 `AsSpan()`/`AsReadOnlySpan()`을, 독립된
managed 복사가 허용될 땐 `ToArray()`/`GetString()`을 쓴다.

---

## `Received.Create()`

caller-provided-storage receive 형태를 위한 재사용 가능한 receive envelope을 만든다.

```csharp
using Received received = Received.Create();
bool ok = dealer.Recv(received);
```

**옵션.** 인자 없음 — `Received`는 public 생성자가 없다, `Create()`만 있다.

**완료 결과.** `Received`를 동기로 반환한다. caller가 소유하며 반드시 dispose해야 한다.
receive API(Sockets category)는 성공적인 호출마다 내부 상태를 덮어쓴다.

**선택 기준.** receive loop·스레드마다 `Received` 하나를 만들어 호출마다 새로
할당하는 대신 재사용한다.

---

## `Received` member

envelope 메타데이터·message part를 읽거나, envelope의 source를 향한 reply/send를
시작한다.

```csharp
if (received.RequestSeq is { } seq)
{
    received.Reply().Message(Message.From("ok")).Submit();
}
Message first = received.FirstPart();
```

**옵션.**

| Member | 반환 | 의미 |
| --- | --- | --- |
| `RoutingId` | `RoutingId?` | receive 경로가 제공할 때만 존재 |
| `RequestSeq` | `ulong?` | reply 가능할 때만 존재 |
| `MessageType` | `ReceivedMessageType` | `Raw`/`Request`/`Reply`/`ErrorReply` |
| `Parts` | `IReadOnlyList<Message>` | 이 envelope이 담은 모든 message part |
| `IsSinglePart` | `bool` | `Parts`가 정확히 하나인지 |
| `FirstPart()` | `Message` | 첫 part, 소유권 이전 없음 |
| `SinglePartOrThrow()` | `Message` | 단일 part, `Parts`가 둘 이상이면 예외 |
| `Reply()` | builder | `RequestSeq`가 값을 가질 때만 유효 — 아래 공유 builder 형태 참고 |
| `Send()` | builder | envelope의 source route로 향함 |

**완료 결과.** 모두 동기다. `Dispose()`는 다른 API가 이미 소유권을 이전하지 않은 한 이
envelope이 소유한 message part를 해제한다. `Reply()`/`Send()`는 아래 공유
operation-builder 형태의 builder를 반환한다.

**선택 기준.** `MessageType`/`RequestSeq`로 분기해 envelope이 reply 가능한지 판단한다.
source route를 따로 찾지 않고 요청에 답하려면 `Reply()`를 쓴다.

---

## `TopicMessage`

수신된 publish 하나 — topic, source routing id, message part를 담는다.

```csharp
using TopicMessage published = new TopicMessage();
bool ok = sub.Recv(published);
string topic = published.Topic;
```

**옵션.**

| Member | 반환 | 의미 |
| --- | --- | --- |
| `TopicMessage()` | — | public 생성자(`Received`와 달리 factory가 아니라 직접 생성) |
| `RoutingId` | `RoutingId?` | 발행자의 routing id, receive 경로가 제공할 때만 존재 |
| `Topic` | `string` | topic byte에서 지연 디코딩 |
| `Parts` | `IReadOnlyList<Message>` | 이 publish가 담은 모든 message part |
| `IsSinglePart` | `bool` | `Parts`가 정확히 하나인지 |
| `FirstPart()` / `SinglePartOrThrow()` | `Message` | `Received`와 같은 형태 — 소유권 이전 없는 첫 part, 또는 단일 part(multipart면 예외) |

**완료 결과.** 동기다. `Dispose()`는 이 instance가 소유한 part를 해제한다.

**선택 기준.** `Received`를 재사용하는 것과 같은 방식으로 subscribe-receive loop
전체에서 instance 하나를 재사용한다.

---

## `SubscriptionEvent`

XPUB socket이 관찰한 구독자 한 명의 subscribe·unsubscribe를 보고한다.

```csharp
using SubscriptionEvent evt = new SubscriptionEvent();
bool ok = xpub.Recv(evt);
```

**옵션.**

| Member | 반환 | 의미 |
| --- | --- | --- |
| `SubscriptionEvent()` | — | public 생성자 |
| `RoutingId` | `RoutingId?` | 구독자의 routing id, receive 경로가 제공할 때만 존재 |
| `Topic` | `string` | 구독·구독 취소된 topic |
| `Subscribed` | `bool` | 구독이면 `true`, 구독 취소면 `false` |
| `SubscriptionEntry(string Filter, bool IsPattern)` | record | 활성 구독 하나 |

**완료 결과.** 동기다. dispose 없음 — 이 타입은 자신의 native resource를 소유하지
않는다.

**선택 기준.** application 레벨 프로토콜 메시지로 구독자 변동을 추론하는 대신, XPUB
socket의 subscription-event receive 경로(Sockets category)에서 이를 관찰한다.

---

## Send / request / reply operation-builder 형태

Sockets category의 모든 `Send`, routed send, `Publish`, `Request`, `Reply` 진입점이
반환하는, part·flag·terminal submit을 누적하는 fluent builder.

```csharp
dealer.Send(routingId).Message(Message.From("part-1")).Message(Message.From("part-2")).Submit();

IReadOnlyList<Message> reply = await dealer
    .Request(routingId)
    .Message(Message.From("payload"))
    .Timeout(TimeSpan.FromSeconds(5))
    .Async();

received.Reply().Message(Message.From("ok")).Submit();
```

**옵션.**

| 단계 | Member | 의미 |
| --- | --- | --- |
| `SendOperation` | `.Message(Message)` | chain 시작 |
| `SendSubmitOperation` | `.Message(...)` / `.Flags(SendFlags)` / `.Submit()` | part 추가, flag 설정, terminal |
| `RequestOperation`/`RequestSubmitOperation` | `Send`와 동일 + `.Timeout(TimeSpan)` | send chain을 그대로 반영하며 reply 대기 timeout을 더함 |
| `RequestSubmitOperation.Flags(...)` | `RequestCallbackSubmitOperation`으로 좁힘 | awaitable `.Async()` 경로가 사라짐 — 이후 `.Submit(RequestCallback)`만 도달 가능 |
| `ReplyOperation`/`ReplySubmitOperation` | `Send`와 같은 형태, flags 단계 없음 | core reply 함수가 send-flag 인자를 받지 않음 |
| `Messages(IReadOnlyList<Message>)` | `MessageOperations` extension | 네 family 전체의 모든 단계에서 여러 part를 순서대로 한 번에 추가; 독립 진입점 아님 |

**완료 결과.** 모두 동기 호출이다; part는 성공적인 submit에서만 소비되며, 실패 시
소유권은 caller에게 복원된다.

| Terminal | 반환 | 의미 |
| --- | --- | --- |
| `SendSubmitOperation.Submit()` | `bool` | `SendFlags.DontWait` backpressure일 때만 `false`, 그 외 실패는 `ZlinkException` |
| `ReplySubmitOperation.Submit()` | `void` | 실패하면 `ZlinkException`을 던짐 |
| `RequestSubmitOperation.Async(CancellationToken)` | `Task<IReadOnlyList<Message>>` | caller가 reply message를 소유하며 반드시 dispose해야 함 |
| `Submit(RequestCallback)`(`RequestSubmitOperation`/`RequestCallbackSubmitOperation`) | `bool` | dispatch 결과일 뿐, 실제 reply는 나중에 콜백으로 `(RequestResult result, IReadOnlyList<Message> parts)`가 전달됨 — `result`가 `RequestResult.Ok`일 때만 `parts`가 채워짐 |

**선택 기준.** async 코드에선 `.Async()`를 쓴다. callback-completion 표면이 필요할 땐
(await할 수 없는 동기 dispatch 스레드) `.Flags(...).Submit(callback)`을 쓴다.
목적지 route를 손으로 재구성하는 대신 `Received` envelope의 `Reply()`/`Send()`를 쓴다.

---

[`Contracts/Messaging/`](../../../../bindings/dotnet/src/Zlink/Contracts/Messaging/)와
[.NET 바인딩 스펙](../../spec/dotnet/README.ko.md)에서 전체 근거를 확인한다.
