한국어 | [English](02-messaging.en.md)

[레퍼런스 목차](README.ko.md)

# 02. Messaging

이 category는 `Message`(frame 소유), 수신 envelope 타입(`Received`,
`TopicMessage`, `SubscriptionEvent`), 그리고 모든 socket type의 진입점이
반환하는 공유 send/request/reply operation-builder interface를 다룬다.
`RoutingID`는 messaging 전용이 아니라 package 전역 value type이므로 Core
category에서 다룬다. 정확한 signature는
[`internal/native/message.go`](../../../../bindings/go/internal/native/message.go),
[`received.go`](../../../../bindings/go/internal/native/received.go),
[`topic_message.go`](../../../../bindings/go/internal/native/topic_message.go),
[`subscription_event.go`](../../../../bindings/go/internal/native/subscription_event.go),
[`operations.go`](../../../../bindings/go/internal/native/operations.go)가
소유하며,
root package의 [`root_projection.go`](../../../../bindings/go/root_projection.go)가
alias로 re-export한다.

---

## `Message`

하나의 native zlink frame을 소유한다 — 모든 send/request/reply/receive
API가 옮기는 단위.

```go
empty, err := zlink.NewMessage(nil)
sized, err := zlink.NewMessageWithSize(4096)
fromBytes, err := zlink.NewMessage(payload)
fromString, err := zlink.NewMessageString("payload")
```

**Options.**

| Member | 의미 |
| --- | --- |
| `NewMessage(data []byte)` | `data`를 복사; `nil`/빈 슬라이스는 길이 0 메시지를 만든다 |
| `NewMessageWithSize(size int)` | 쓰기 가능한, 0으로 초기화된 storage |
| `NewMessageString(value string)` | UTF-8 인코딩 후 복사 |
| `Data() []byte` | `Close`까지만 유효한 view — 여러 goroutine에 걸쳐 또는 메시지 수명을 넘어 보관하지 **말 것**, 그럴 땐 `Bytes()`를 사용한다 |
| `Bytes() []byte` | payload의 스냅샷 복사 |
| `Size()` | payload 바이트 길이 |
| `IsEmpty()` | `Size()`가 0인지 |
| `Text()` / `String()` | 둘 다 `Data()`를 UTF-8로 디코딩 |
| `CopyTo(destination []byte) (int, error)` | payload를 caller가 제공한 slice로 복사; `destination`이 너무 작으면 error |
| `TryCopyTo(destination []byte) bool` | error를 버리는 `CopyTo`의 non-erroring 버전 |
| `RefCount() int` | 진단 전용; 실패 시 error를 전파하는 대신 `0`을 반환 |
| `Clone()` / `Copy()` | 동등 — 둘 다 `(*Message, error)`, 독립된 payload 복사본 |
| `Close() error` | message를 해제 |

**Completion result.** 모든 member는 동기다. `Close()`는 `nil` receiver나
이미 닫힌 메시지에서 `nil`을 반환한다. 사용이 끝나면 caller가 메시지를 닫는다.

`Message(...)`로 전달한 메시지는 Core가 연산을 수락하면 소비된다. send와 request는 Core가 유효한
WRITABLE 대기 토큰을 반환해도 메시지를 소비한다. 어느 결과도 받기 전에 첫 제출이 실패하면 메시지는
호출자에게 남는다. `MoveMessage(...)`는 제출할 때 소유권을 이전한다.

**선택 기준.** outbound payload는 `NewMessageWithSize`나 `NewMessage`로 만든다.
독립된 메시지가 필요하면 `Clone()`을 사용한다. 열린 메시지의 payload를 참조할 때는
`Data()`를 사용하고, 메시지를 닫은 뒤에도 payload가 필요하면 `Bytes()`로 복사한다.


---

## `Received`

수신 메시지 envelope: routing metadata, ROUTER request의 opaque reply token,
message part. `recv` 호출마다
새로 만드는 대신 인스턴스 하나를 재사용해 매 수신당 할당을 피한다 —
socket의 receive 메서드가 이를 리셋하고 다시 채운다.

```go
var received zlink.Received
ok, err := router.Recv(&received, zlink.RecvFlagsNone)
if _, replyable := received.ReplyToken(); ok && replyable {
    received.Reply().Message(reply).Submit(ctx)
}
```

**Options.** 공개 생성자 없음 — caller가 zero-value `Received{}`를
선언하고 그 주소를 socket의 receive 메서드(Sockets category)에
넘기면 그것이 채운다.

| Member | 의미 |
| --- | --- |
| `RoutingID() RoutingID` / `HasRoutingID() bool` | peer routing id와 존재 여부 |
| `ReplyToken() (ReplyToken, bool)` | opaque reply capability와 존재 여부 |
| `IsSinglePart() bool` | `Parts()`가 정확히 하나인지 |
| `Parts() []*Message` | 이 envelope이 담은 모든 message part |
| `FirstPart() (*Message, error)` | 첫 part는 `Received`가 소유한다. 비어 있으면 `RecvInvalidState`와 `EINVAL`을 담은 `*RecvError`를 반환한다. |
| `Reply() ReplyOp` | 공유 reply builder 시작; `ReplyToken()`이 있을 때만 유효 |
| `Send() SendOp` | 공유 send builder를 시작, 이 envelope이 캡처한 source route로 향함 |
| `Close() error` | 보관 중인 모든 part를 닫음; `nil` receiver에서 호출해도 안전 |

**Completion result.** 모든 member는 동기다. `Close()`(그리고 receive
메서드 자신의 reset 단계)는 이전에 보관 중이던 모든 part를 버리기
전에 닫는다 — 닫지 않고 `Received`를 재사용해도 native frame이 새는
일은 없다.

**선택 기준.** 수신 loop에서 매 메시지당 새로 할당하는 대신 `Received`
하나를 참조로 재사용한다. 목적지 route를 손으로 재구성하는 대신
`Reply()`를 사용한다 — routing id와 opaque reply token은 캡슐화돼 있어 이
builder 밖에선 접근할 수 없다.

---

## `TopicMessage`

수신된 publish: topic, source routing id, message part. `Received`와
같은 방식으로 subscribe-receive 호출마다 인스턴스 하나를 재사용한다.

```go
var published zlink.TopicMessage
ok, err := sub.Subscribe(&published, zlink.RecvFlagsNone)
topic := published.Topic()
```

**Options.** 공개 생성자 없음 — zero-value `TopicMessage{}`를 선언한다.

| Member | 의미 |
| --- | --- |
| `RoutingID() RoutingID` / `HasRoutingID() bool` | publisher의 routing id와 존재 여부 |
| `Topic() string` | 이 publish가 전송된 topic |
| `IsSinglePart() bool` / `Parts() []*Message` / `FirstPart() (*Message, error)` / `Close() error` | `Received`와 같은 형태 |

**Completion result.** 동기다; `Received`와 동일한 close-후-재사용
계약.

**선택 기준.** `Received`와 같은 방식으로 subscribe-receive loop에서
인스턴스 하나를 재사용한다.

---

## `SubscriptionEvent`

XPUB socket이 관찰한, 한 subscriber의 subscribe 또는 unsubscribe를
보고한다.

```go
var evt zlink.SubscriptionEvent
ok, err := xpub.ReceiveSubscriptionEvent(&evt, zlink.RecvFlagsNone)
```

**Options.** 공개 생성자 없음 — zero-value `SubscriptionEvent{}`를
선언한다. 모든 member는 value receiver다.

| Member | 의미 |
| --- | --- |
| `RoutingID() RoutingID` / `HasRoutingID() bool` | 구독자의 routing id와 존재 여부 |
| `Subscribed() bool` | subscribe면 `true`, unsubscribe면 `false` |
| `Topic() string` | subscribe/unsubscribe된 topic |

**Completion result.** 동기다; `Close()` 없음 — 이 타입은 자신만의
native resource를 소유하지 않는다(message part를 소유하는
`Received`/`TopicMessage`와 다름).

**선택 기준.** subscriber 이탈을 관찰하려고 XPUB socket의
subscription-event receive 경로(Sockets category)에서 사용한다.

---

## Send / request / reply operation-builder 형태

`SendOp`, `RequestOp`, `ReplyOp`, `PublishOp`에서 첫 part를 추가한 뒤
`Submit(ctx)`를 호출한다. send, request, publish는 `Message(*Message)`나
`Bytes([]byte)`로 part를 받는다.
send와 publish는 `MoveMessage(*Message)`, request는 `Timeout(time.Duration)`, publish는
`Flags(SendFlags)`를 추가로 제공한다. reply의 part는 `Message(*Message)`로 추가한다.

```go
send, err := dealer.Send().Message(part1).Message(part2).Submit(ctx)
if err != nil {
    return err
}
if err := send.Admitted(ctx); err != nil {
    return err
}
```

```go
request, err := dealer.Request().Message(payload).Timeout(5 * time.Second).Submit(ctx)
if err != nil {
    return err
}
parts, err := request.Reply(ctx)
if err != nil {
    return err
}
for _, part := range parts {
    defer part.Close()
}
```

```go
if err := received.Reply().Message(reply).Submit(ctx); err != nil {
    return err
}
```

**Options.** 첫 part를 추가하면 builder가 해당 submit interface를 반환한다.
이미 취소됐거나 deadline이 지난 `context.Context`를 넘기면 native submit 전에
`ctx.Err()`를 반환한다.

| Terminal | 반환값 | 의미 |
| --- | --- | --- |
| send `Submit(ctx)` | `(SendSubmission, error)` | `Admitted(ctx)`로 수락을 기다린다. |
| request `Submit(ctx)` | `(RequestSubmission, error)` | `Admitted(ctx)`로 수락을 기다린다. `Reply(ctx)`는 응답을 기다리고 반환한 part의 소유권을 호출자에게 넘긴다. |
| reply `Submit(ctx)` | `error` | 수락 결과를 반환한다. |
| publish `Submit(ctx)` | `(bool, error)` | `SendFlagsDontWait`에서 `false, nil`은 송신 압력을 뜻한다. |

**Completion result.** send와 request의 `Result()`는 최초 submit 결과를 반환한다.
caller는 `Reply(ctx)`가 반환한 각 part를 닫는다. native submit을 시도한 builder는 다시
제출하면 오류를 반환한다. 제출 뒤 context를 취소하면 Go 대기가 끝날 수 있으며
completion 소유자는 Core completion을 계속 처리한다.

**선택 기준.** `Submit(ctx)`의 반환값과 오류를 먼저 처리한다. public poller에
`PollCompletion`을 등록했다면 수락이나 reply를 기다리는 동안 별도 goroutine에서 그
poller를 구동한다. 수신 envelope에 저장된 source route로 보내려면 `Received.Send()`를
사용하고, reply token이 있는 request에 응답하려면 `Received.Reply()`를 사용한다.

---

[`internal/native/message.go`](../../../../bindings/go/internal/native/message.go),
[`received.go`](../../../../bindings/go/internal/native/received.go),
[`operations.go`](../../../../bindings/go/internal/native/operations.go),
[Go 바인딩 스펙](../../spec/go/README.ko.md)에서 전체 근거를 확인한다.
