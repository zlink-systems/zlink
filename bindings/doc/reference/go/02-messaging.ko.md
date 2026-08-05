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
[`contracts/messaging.go`](../../../../bindings/go/contracts/messaging.go)를
통해 alias로 re-export된다.

---

## `Message`

하나의 native zlink frame을 소유한다 — 모든 send/request/reply/receive
API가 옮기는 단위.

```go
empty, err := contracts.NewMessage(nil)
sized, err := contracts.NewMessageWithSize(4096)
fromBytes, err := contracts.NewMessage(payload)
fromString, err := contracts.NewMessageString("payload")
```

**Options.**

| Member | 의미 |
| --- | --- |
| `NewMessage(data []byte)` | `data`를 복사; `nil`/빈 슬라이스는 길이 0 메시지를 만든다 |
| `NewMessageWithSize(size int)` | 쓰기 가능한, 0으로 초기화된 storage |
| `NewMessageString(value string)` | UTF-8 인코딩 후 복사 |
| `Data() []byte` | `Close`까지만 유효한 view — 여러 goroutine에 걸쳐 또는 메시지 수명을 넘어 보관하지 **말 것**, 그럴 땐 `Bytes()`를 쓴다 |
| `Bytes() []byte` | payload의 스냅샷 복사 |
| `Size()` | payload 바이트 길이 |
| `IsEmpty()` | `Size()`가 0인지 |
| `Text()` / `String()` | 둘 다 `Data()`를 UTF-8로 디코딩 |
| `CopyTo(destination []byte) (int, error)` | payload를 caller가 제공한 slice로 복사; `destination`이 너무 작으면 error |
| `TryCopyTo(destination []byte) bool` | error를 버리는 `CopyTo`의 non-erroring 버전 |
| `RefCount() int` | 진단 전용; 실패 시 error를 전파하는 대신 `0`을 반환 |
| `Clone()` / `Copy()` | 동등 — 둘 다 `(*Message, error)`, 독립된 payload 복사본 |
| `Close() error` | message를 해제 |

**Completion result.** 모든 member는 동기다. `Close()`는 멱등이다(`nil`
receiver나 이미 닫힌 메시지는 `nil`을 반환); finalizer가 없으므로
caller가 명시적으로 호출해야 한다. `MoveMessage` 계열 builder 호출로
메시지를 보내면 native frame이 socket으로 이전되고 Go 쪽에서 closed로
표시된다(아래 operation-builder 형태 참고); 일반 `Message` 계열
호출로 보내면 caller의 `Message`는 여전히 소유 상태고 여전히
`Close()`가 필요하다.

**선택 기준.** outbound payload를 만들 땐 `NewMessageWithSize`/
`NewMessage`를 쓴다; `MoveMessage`로 소유권을 이전하는 대신 독립
복사본이 필요할 땐 `Clone()`을 쓴다. 다음 `recv` 호출 전에 슬라이스를
다 쓰는 hot path에선 `Bytes()`보다 `Data()`를 우선한다 —
`Bytes()`는 항상 복사를 할당한다.

---

## `Received`

수신 메시지 envelope: routing metadata, request sequence(이 수신이
request/reply 교환에서 온 경우), message part. `recv` 호출마다
새로 만드는 대신 인스턴스 하나를 재사용해 매 수신당 할당을 피한다 —
socket의 receive 메서드가 이를 리셋하고 다시 채운다.

```go
var received contracts.Received
ok, err := dealer.Recv(&received, contracts.RecvFlagsNone)
if ok && received.HasRequestSeq() {
    received.Reply().Message(reply).Submit(ctx)
}
```

**Options.** 공개 생성자 없음 — caller가 zero-value `Received{}`를
선언하고 그 주소를 socket의 receive 메서드(Sockets category)에
넘기면 그것이 채운다.

| Member | 의미 |
| --- | --- |
| `RoutingID() RoutingID` / `HasRoutingID() bool` | peer routing id와 존재 여부 |
| `RequestSeq() uint64` / `HasRequestSeq() bool` | request sequence와 존재 여부 |
| `IsSinglePart() bool` | `Parts()`가 정확히 하나인지 |
| `Parts() []*Message` | 이 envelope이 담은 모든 message part |
| `FirstPart() (*Message, error)` | 소유권 이전 없이 첫 part — part는 여전히 `Received`가 소유 |
| `Reply() ReplyOp` | 공유 reply builder를 시작; `HasRequestSeq()`가 true일 때만 유효 — 아니면 결과 builder의 `Submit`이 `*SubmitError`를 반환 |
| `Send() SendOp` | 공유 send builder를 시작, 이 envelope이 캡처한 source route로 향함 |
| `Close() error` | 보관 중인 모든 part를 닫음; `nil` receiver에서 호출해도 안전 |

**Completion result.** 모든 member는 동기다. `Close()`(그리고 receive
메서드 자신의 reset 단계)는 이전에 보관 중이던 모든 part를 버리기
전에 닫는다 — 닫지 않고 `Received`를 재사용해도 native frame이 새는
일은 없다.

**선택 기준.** 수신 loop에서 매 메시지당 새로 할당하는 대신 `Received`
하나를 참조로 재사용한다. 목적지 route를 손으로 재구성하는 대신
`Reply()`를 쓴다 — routing id와 request sequence는 캡슐화돼 있어 이
builder 밖에선 접근할 수 없다.

---

## `TopicMessage`

수신된 publish: topic, source routing id, message part. `Received`와
같은 방식으로 subscribe-receive 호출마다 인스턴스 하나를 재사용한다.

```go
var published contracts.TopicMessage
ok, err := sub.Subscribe(&published, contracts.RecvFlagsNone)
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
var evt contracts.SubscriptionEvent
ok, err := xpub.ReceiveSubscriptionEvent(&evt, contracts.RecvFlagsNone)
```

**Options.** 공개 생성자 없음 — zero-value `SubscriptionEvent{}`를
선언한다. 모든 member는 value receiver다, pointer가 아니다.

| Member | 의미 |
| --- | --- |
| `RoutingID() RoutingID` / `HasRoutingID() bool` | 구독자의 routing id와 존재 여부 |
| `Subscribed() bool` | subscribe면 `true`, unsubscribe면 `false` |
| `Topic() string` | subscribe/unsubscribe된 topic |

**Completion result.** 동기다; `Close()` 없음 — 이 타입은 자신만의
native resource를 소유하지 않는다(message part를 소유하는
`Received`/`TopicMessage`와 다름).

**선택 기준.** subscriber 이탈을 관찰하려고 XPUB socket의
subscription-event receive 경로(Sockets category)에서 쓴다.

---

## Send / request / reply operation-builder 형태

모든 socket type의 `Send`/`Publish`/`Request`/`Reply` 진입점(Sockets
category)이 반환하는, part·flag·terminal submit을 누적하는 fluent
builder interface. 각 단계는 별개의 Go interface다(`SendOp` →
`SendSubmitOp`, `RequestOp` → `RequestSubmitOp` →
`RequestCallbackSubmitOp`, `ReplyOp` → `ReplySubmitOp`) — 단계에
맞는 메서드를 호출하면 chain의 다음 interface를 반환하므로, caller는
part를 하나라도 추가하기 전엔 `Submit`을 호출할 수 없다.

```go
ok, err := dealer.Send().Message(part1).Message(part2).Submit(ctx)

ch, err := dealer.Request().
    Message(payload).
    Timeout(5 * time.Second).
    SubmitAsync(ctx)
completion := <-ch

ok, err := dealer.Request().
    Message(payload).
    Submit(ctx, func(result contracts.RequestResult, parts []*contracts.Message) {
        // 나중에, 백그라운드 dispatch goroutine에서 전달됨
    })

err := received.Reply().Message(reply).Submit(ctx)
```

**Options.** **모든 terminal `Submit`/`SubmitAsync`는 첫 인자로
`context.Context`를 받는다** — 취소되거나 deadline이 지난 context는
native submit이 실행되기 전에 그 context의 `Err()`로 호출을 즉시
중단시킨다, 다른 어떤 언어의 operation builder도 이러지 않는다.

| Stage | Member | 의미 |
| --- | --- | --- |
| `SendOp` | `.Message(*Message)` / `.MoveMessage(*Message)` / `.Bytes([]byte)` → `SendSubmitOp` | chain을 시작; `MoveMessage`는 submit이 성공하면 메시지 소유권을 socket으로 이전 |
| `SendSubmitOp` | 같은 세 추가 메서드 + `.Flags(SendFlags)` / `.Submit(ctx context.Context) (bool, error)` | part 추가, flag 설정, terminal |
| `RequestOp` | `.Message` / `.Bytes` → `RequestSubmitOp` | request chain을 시작 |
| `RequestSubmitOp` | `.Timeout(time.Duration)` | reply-wait timeout을 더함 |
| `RequestSubmitOp.Flags(SendFlags)` | `RequestCallbackSubmitOp`로 좁혀짐 | callback 전용 단계 |
| `RequestSubmitOp` terminal | `.SubmitAsync(ctx) (<-chan RequestReplyCompletion, error)`와 `.Submit(ctx, callback RequestReplyCallback) (bool, error)` | **동시에 둘 다 노출** — 지금까지 다룬 다른 어떤 언어도 같은 builder 단계에 async-channel과 callback terminal을 동시에 노출하지 않는다 |
| `RequestCallbackSubmitOp` | `Message`/`Bytes`/`Timeout`/`Flags`를 반복 + callback `Submit`만 | 좁혀지면 `SubmitAsync` channel 경로는 사라진다 |
| `ReplyOp` | `.Message(*Message)` → `ReplySubmitOp` | reply chain을 시작 |
| `ReplySubmitOp` | `.Flags(SendFlags)` / `.Submit(ctx context.Context) error` | flag 설정, terminal |

**Completion result.** `SendSubmitOp.Submit`은 `(bool, error)`를
반환한다 — `bool`은 `SendFlagsDontWait`가 설정됐고 send가 block됐을
경우에만(backpressure) `false`다; 그 외 실패는 전부 `error`다.
`ReplySubmitOp.Submit`은 `error`만 반환한다. `RequestSubmitOp.Submit`/
`RequestCallbackSubmitOp.Submit`은 *dispatch* 자체에 대해(request가
수락됐는지) `(bool, error)`를 반환한다 — 실제 reply나 실패는 나중에
`RequestReplyCompletion`(`{Result RequestResult; Parts []*Message; Err
error}`)로 `SubmitAsync` channel에 실려 오거나, `Submit` callback의
`(RequestResult, []*Message)` 인자로 오며, 어느 쪽이든 백그라운드
dispatch goroutine에서 전달된다. 모든 builder는 1회용이다 — 같은
builder 값에 `Submit`/`SubmitAsync`를 두 번째 호출하면 재제출하는
대신 error를 반환한다.

**선택 기준.** 호출부가 이미 channel/`select`로 통신할 땐
`SubmitAsync`를 쓴다; callback을 기대하는 코드와 통합할 땐 callback
`Submit`을 쓴다. 목적지 route를 손으로 재구성하는 대신
`Received.Reply()`/`Send()`를 쓴다 — routing id(그리고 `Reply`의
경우 request sequence)는 캡슐화돼 있어 builder 밖에선 접근할 수 없다.

---

[`internal/native/message.go`](../../../../bindings/go/internal/native/message.go),
[`received.go`](../../../../bindings/go/internal/native/received.go),
[`operations.go`](../../../../bindings/go/internal/native/operations.go),
[Go 바인딩 스펙](../../spec/go/README.ko.md)에서 전체 근거를 확인한다.
