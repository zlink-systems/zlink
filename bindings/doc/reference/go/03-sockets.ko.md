한국어 | [English](03-sockets.en.md)

[레퍼런스 목차](README.ko.md)

# 03. Sockets

Go 바인딩은 여덟 종류의 socket을 제공한다. socket 생성은 [Core 레퍼런스](01-core.ko.md),
operation builder는 [Messaging 레퍼런스](02-messaging.ko.md)를 참조한다.
아래 예제에서 `ctx`는 `*zlink.Context`이고 `callCtx`는 Go의 `context.Context`다.

---

## 공통 socket 메서드

| Member | 의미 |
| --- | --- |
| `Bind(endpoint string) error` / `Unbind(endpoint string) error` | 주소에서 listen을 시작/중단 |
| `Connect(endpoint string) error` / `Disconnect(endpoint string) error` | peer 주소로 connect/disconnect |
| `DisconnectRID(peerRID RoutingID) error` | 해당 routing id로 식별되는 peer를 disconnect |
| `Close() error` | native socket을 닫는다 |
| `CommonOptions() *CommonSocketOptions` | 공유 option facade |
| `LastEndpoint() (string, error)` | 실제로 해석된 bind 주소 |
| `SetTLSServer(certPath, keyPath string, requireClientCert bool) error` | 결합된 서버측 TLS 설정, `Bind` 전에 적용 |
| `SetTLSClient(caCertPath, hostname string, trustSystem bool) error` | 결합된 클라이언트측 TLS 설정, `Connect` 전에 적용 |

모든 socket은 `Bind`, `Unbind`, `DisconnectRID`, `Close`, `LastEndpoint`, `CommonOptions`와
두 TLS 설정 메서드를 제공한다. 나가는 연결을 지원하는 일곱 socket은 `Connect`, `Disconnect`,
`ConnectionAttemptsSnapshot`도 제공한다.

**Completion result.** 수명주기 method와 TLS setter는 `error`를 반환한다.
`LastEndpoint()`는 `(string, error)`를 반환한다. `CommonOptions()`는 option
facade를 바로 반환한다.

**선택 기준.** raw TCP peer의 연결을 받으려면 STREAM을 사용한다. `Connect`가 필요하면
나가는 연결을 지원하는 socket을 선택한다.
나가는 연결의 준비 상태는 [연결 시도 스냅샷](04-eventing.ko.md#연결-시도-스냅샷)을 참조한다.

---

## `CommonSocketOptions` / `PubSocketOptions`

모든 socket은 `CommonOptions()`로 `CommonSocketOptions`를 제공한다.
PUB와 XPUB는 `PubOptions()`로 `PubSocketOptions`도 제공한다.

```go
opts := dealer.CommonOptions()
opts.SetSendHighWaterMark(100_000)
opts.SetLinger(time.Second)
opts.SetSubmitRetryMode(zlink.SubmitRetryLocalFailure)
```

**Options — `CommonSocketOptions`.** 모든 accessor가 `(T, error)`/`error`를
반환한다.

| Member | 의미 |
| --- | --- |
| `Linger()` / `SetLinger(time.Duration)` | `Close()`가 대기 중인 send를 flush하기 위해 기다리는 상한 |
| `SendHighWaterMark()` / `SetSendHighWaterMark(uint64)` | outbound accounted-byte HWM |
| `ReceiveHighWaterMark()` / `SetReceiveHighWaterMark(uint64)` | inbound accounted-byte HWM |
| `SendTimeout()` / `SetSendTimeout(time.Duration)` | blocking send가 기다리는 상한 |
| `ReceiveTimeout()` / `SetReceiveTimeout(time.Duration)` | blocking receive가 기다리는 상한 |
| `Immediate()` / `SetImmediate(bool)` | send가 지금 당장 살아있는 연결을 요구하는지, 아니면 연결이 생길 때까지 큐잉하는지 |
| `RidDuplicatePolicy()` / `SetRidDuplicatePolicy(RidDuplicatePolicy)` | peer가 기존 routing id를 재사용할 때 벌어지는 일 |
| `ConnectTimeout()` / `SetConnectTimeout(time.Duration)` | connect handshake가 기다리는 상한 |
| `IPv6()` / `SetIPv6(bool)` | socket이 IPv6 연결을 받아들이는지 |
| `TCPNoDelay()` / `SetTCPNoDelay(bool)` | `true`면 Nagle 알고리즘을 비활성화 |
| `TCPKeepalive()` / `SetTCPKeepalive(bool)` | OS TCP keepalive 모드 |
| `MaxMessageSize()` / `SetMaxMessageSize(int64)` | 수신 허용하는 메시지 한 건의 최대 바이트 크기 |
| `Backlog()` / `SetBacklog(int)` | listening socket의 대기 연결 큐 길이 |
| `ReconnectInterval()` / `SetReconnectInterval(time.Duration)` | 재연결 시도 사이 간격 |
| `ReconnectIntervalMax()` / `SetReconnectIntervalMax(time.Duration)` | 재연결 간격의 상한 |
| `SubmitRetryMode()` / `SetSubmitRetryMode(SubmitRetryMode)` | local back-pressure에서 실패한 submit이 자동 재시도되는지 |
| `SubmitRetryTimeout()` / `SetSubmitRetryTimeout(time.Duration)` | `SubmitRetryMode()`가 `SubmitRetryLocalFailure`일 때의 재시도 timeout |
| `SubmitRetryAttempts()` / `SetSubmitRetryAttempts(int)` | `SubmitRetryMode()`가 `SubmitRetryLocalFailure`일 때의 재시도 횟수 상한 |
| `LastEndpoint()` | 읽기 전용, socket에 직접 위임 — 실제로 해석된 bind 주소 |

**Options — `PubSocketOptions`.**

| Member | 의미 |
| --- | --- |
| `NoDrop()` / `SetNoDrop(bool)` | back-pressure에서 조용히 버리는 대신 오류 |
| `Verbose()` / `SetVerbose(bool)` / `Verboser()` / `SetVerboser(bool)` | 중복 포함 모든 (un)subscribe 메시지를 전달 |
| `Manual()` / `SetManual(bool)` | 구독이 자동 승인 대신 `ApproveSubscribe`/`RejectSubscribe`를 요구 |
| `ManualLastValue()` / `SetManualLastValue(bool)` | manual 모드에 더해 새로 승인된 구독자에게 topic별 마지막 캐시 메시지도 재전송 |
| `TopicsCount() (int, error)` | 읽기 전용, 활성 구독 개수 |
| `WelcomeMessage()` / `SetWelcomeMessage(*Message)` | 새로 연결된 구독자 각각에게 자동 전송 |
| `ApproveSubscribe(RoutingID) error` / `RejectSubscribe(RoutingID) error` | set-only — 대응하는 getter 없음 |

**Completion result.** 모든 accessor는 동기다, 값과 함께 `error`를
반환한다 — Core category의 `Context.Options()` 관례와 일관됨.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 땐 socket이 메시지 교환을
시작하기 전에 `SendHighWaterMark`/`ReceiveHighWaterMark`와 `Linger`를
설정한다. `PubSocketOptions`는 `CommonOptions()`가 아니라
`PubSocket`/`XPubSocket`을 통해 직접 접근한다(둘 다 `PubOptions()`를
선언하는 `publishSocket`을 embed한다).

---

## `PairSocket`

routing 없는 배타적 1:1 peering socket.

```go
pair, err := ctx.PairSocket()
pair.Send().Message(ping).Submit(callCtx)
var received zlink.Received
ok, err := pair.Recv(&received, zlink.RecvFlagsNone)
```

**Options.** 위 공유 lifecycle/TLS/option 표면에 더해:

| Member | 의미 |
| --- | --- |
| `Send() SendOp` | 공유 send builder를 시작 |
| `Recv(out *Received, flags RecvFlags) (bool, error)` | `out`을 다음 메시지로 채움 |
| send `Submit(ctx)` | `(SendSubmission, error)`를 반환한다. Core가 수락 전 재시도를 처리하는 동안 `Admitted(ctx)`로 수락을 기다린다. |

**Completion result.** `Recv`는 `RecvFlagsDontWait`가 설정돼 있고
메시지가 없을 때만 `(false, nil)`을 반환한다.

**선택 기준.** 배타적 point-to-point 링크엔 PAIR를 사용한다 — peer routing이
없고 load-balance하지 않는다.

---

## `DealerSocket`

연결된 peer 전체에 send를 load-balance하고 routed request를 발행할 수
있다.

```go
dealer, err := ctx.DealerSocket()
dealer.SetRoutingID(zlink.NewRoutingIDString("worker-3"))
request, err := dealer.Request().Message(payload).Submit(callCtx)
if err != nil {
    return err
}
parts, err := request.Reply(callCtx)
if err != nil {
    return err
}
defer zlink.MultipartClose(parts)
```

**Options.** `PairSocket`과 같은 공유 표면에 더해:

| Member | 의미 |
| --- | --- |
| `Send() SendOp` | 공유 send builder를 시작 |
| `Recv(out, flags) (bool, error)` | `out`을 다음 메시지로 채움 |
| `Request() RequestOp` | 공유 request builder를 시작; target 인자 없음 — DEALER엔 API 레벨 peer routing id가 없기 때문 |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `SetProbe(bool) error` / `Probe() (bool, error)` | 연결 시 빈 probe를 보낼지 설정하거나 읽는다. |
| `Weight()` / `SetWeight(int)` | load-balancing 가중치, 양방향 모두 |
| `SetRequestTimeout(time.Duration) error` / `RequestTimeout() (time.Duration, error)` | request 제한 시간을 설정하거나 읽는다. |

**Completion result.** `Recv`는 `PairSocket`과 같은 `DontWait`에서
`(false, nil)` 관례를 따른다.

**선택 기준.** peer가 첫 메시지부터 관찰하도록 connect 전에
`SetRoutingID`를 설정한다. DEALER엔 임의 token에 응답하는 프로토콜
envelope helper가 없다 — 대신 수신 request context의
`Received.Reply()`를 사용하거나 ROUTER의 명시적 reply 표면을 사용한다.

---

## `RouterSocket`

routing id로 지정된 peer에 메시지를 보내고 특정 peer의 request에 응답한다.

```go
router, err := ctx.RouterSocket()
router.SendTo(peerRID).Message(hello).Submit(callCtx)
router.Reply(peerRID, replyToken).Message(reply).Submit(callCtx)
```

**Options.** `PairSocket`과 같은 공유 표면에 더해:

| Member | 의미 |
| --- | --- |
| `SendTo(target RoutingID) SendOp` | 공유 send builder를 그 peer로 향해 시작 |
| `Recv(out, flags) (bool, error)` | `out`을 다음 application message로 채움 |
| `Request(peerRID RoutingID) RequestOp` | Messaging category의 `RequestOp`, 특정 peer로 향함 |
| `Reply(rid RoutingID, token ReplyToken) ReplyOp` | opaque token이 식별하는 request에 답하는 `ReplyOp` |
| `Received.ReplyToken()` | ROUTER receive가 제공한 socket-bound token 반환 |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `SetMandatory(bool) error` / `Mandatory() (bool, error)` | 알 수 없는 route에서 송신 오류를 반환할지 설정하거나 읽는다. |
| `SetProbe(bool) error` / `Probe() (bool, error)` | 연결 시 빈 probe를 보낼지 설정하거나 읽는다. |
| `SetHandover(bool) error` / `Handover() (bool, error)` | 공통 routing ID 중복 정책을 읽거나 설정한다. `true`는 `RidDuplicateHandover`, `false`는 `RidDuplicateReject`에 대응한다. |
| `SetConnectRoutingID(RoutingID) error` | 다음 연결에 사용할 로컬 별칭을 설정한다. |
| `Weight()` / `SetWeight(int)` | load-balancing 가중치, 양방향 모두 |
| `SetRequestTimeout(time.Duration) error` / `RequestTimeout() (time.Duration, error)` | request 제한 시간을 설정하거나 읽는다. |
| request `Submit(ctx)` | 제출 결과를 반환한다. 응답 part나 최종 오류는 `Reply(ctx)`로 받는다. |
| send `Submit(ctx)` | 제출 결과를 반환한다. `Admitted(ctx)`로 수락을 기다린다. |

**Completion result.** `Recv`는 `PairSocket`과 같은 관례를 따른다.

**선택 기준.** DEALER가 특정 peer를 지정할 수 없는 곳에서
ROUTER-시작 또는 ROUTER-응답 request/reply엔
`Request(peerRID)`/`Reply(rid, token)`을 사용한다. Token은 receive에서 얻고 합성하거나
재사용하지 않는다.

---

## `PubSocket` / `SubSocket` / `XPubSocket` / `XSubSocket`

PUB는 topic-filtered 메시지를 발행하며 매칭되는 subscriber가 없으면
버린다; SUB는 socket option으로 설정된 subscription으로 구독한다;
XPUB는 subscriber-event 노출을 추가하고, XSUB는 대신 subscription을
메시지로 실어 나른다. `PubSocket`/`XPubSocket`은 둘 다 `publishSocket`을
embed한다; `SubSocket`/`XSubSocket`은 둘 다 `subscribeSocket`을
embed한다.

```go
pubSocket, err := ctx.PubSocket()
pubSocket.Publish("prices").Message(tick).Submit(callCtx)

sub, err := ctx.SubSocket()
sub.SetSubscription("prices.")
var msg zlink.TopicMessage
ok, err := sub.Subscribe(&msg, zlink.RecvFlagsNone)
```

**Options.** **`PubSocket`도 `XPubSocket`도 `SetRoutingID`/`RoutingID`를
선언하지 않는다** — 둘 다 이 binding에 routing-id 표면이 전혀 없다,
지금까지 다룬 다른 모든 언어와 동일하다. `SubSocket`과 `XSubSocket`은
receive/subscription 표면만 노출하며 managed send completion은 없다.

| 타입 | Member | 의미 |
| --- | --- | --- |
| `PubSocket` | `Publish(topic string) PublishOp` | publish builder를 시작 |
| | `PublishSubmitOp.Submit(ctx) (bool, error)` | synchronous lossy/NODROP publish terminal |
| | `PubOptions() *PubSocketOptions` | 타입별 option facade |
| `SubSocket` / `XSubSocket` | `Subscribe(out *TopicMessage, flags RecvFlags) (bool, error)` | `out`을 다음 매칭 publish로 채움 |
| | `SetSubscription(filter string) error` / `UnsetSubscription(filter string) error` | topic filter를 추가/제거; subscription은 누적된다 |
| | `SubscriptionAt(index int) (Subscription, bool, error)` | 해당 index의 filter와 pattern 여부를 `Subscription{Filter string; Pattern bool}`로 반환한다. Core가 `ENOENT`를 반환하면 결과는 `(Subscription{}, false, nil)`이다 |
| | `TopicsCount() (int, error)` | 활성 구독 개수 |
| `XPubSocket` | `ReceiveSubscriptionEvent(out *SubscriptionEvent, flags RecvFlags) (bool, error)` | `out`을 다음 subscribe/unsubscribe로 채움, `publishSocket`을 통해 embed하는 `PubSocket` 표면 위에 추가됨 |
| | `Publish(topic string) PublishOp` | publish builder를 시작 |

바인딩은 자체 버퍼를 넘겨 `zlink_subscription_at`를 호출한다. 빈 filter를 포함해 성공하면
조회를 끝낸다. Core가 `ENOBUFS`를 반환하면 Core가 알려 준 길이로 한 번 더 호출한다.
그 밖의 Core 실패는 `*ConfigError`로 반환한다.

**Completion result.** `Subscribe`/`ReceiveSubscriptionEvent`는 같은
`DontWait`에서 `(false, nil)` 관례를 따른다.

**선택 기준.** subscriber 이탈을 관찰하려고 특별히 `XPubSocket`을
`ReceiveSubscriptionEvent`와 함께 사용하거나, `PubSocketOptions.SetManual`/
`ApproveSubscribe`/`RejectSubscribe`로 수동 admission을 사용한다.
subscription을 일반 메시지로 실어야 할 때 특별히 `XSubSocket`을 사용한다.

---

## `StreamSocket`

STREAM은 raw TCP peer와 byte record 또는 framed packet을 주고받는다.

```go
stream, err := ctx.StreamSocket()
stream.CommonOptions().SetReceiveHighWaterMark(100_000)
stream.SetReceiveMode(zlink.StreamReceivePacket)
var packet zlink.StreamPacket
ok, err := stream.RecvPacket(&packet, zlink.RecvFlagsNone)
```

`CommonOptions()`로 HWM, timeout, linger 등 공통 옵션을 설정하고 조회한다. 기존
`SetSendHighWaterMark` 같은 직접 메서드도 같은 옵션을 사용한다. STREAM에서 Core가
지원하지 않는 옵션은 `*ConfigError`로 Core 결과 코드와 errno를 전달한다.
`SetNotify`와 `SetReceiveMode`는 STREAM에서 직접 호출한다.

| Member | 의미 |
| --- | --- |
| `SendTo(target RoutingID) SendOp` | 공유 send builder를 그 peer로 향해 시작 |
| `Recv(out *Received, flags RecvFlags) (bool, error)` | `StreamReceiveRaw`에서 raw record pull |
| `RecvPacket(out *StreamPacket, flags RecvFlags) (bool, error)` | `StreamReceivePacket`에서 caller-owned header/body packet 하나 pull |
| `SetReceiveMode(StreamReceiveMode) error` / `ReceiveMode() (StreamReceiveMode, error)` | bind 전에 RAW 또는 PACKET을 선택한다. bind 후에는 mode가 고정된다. |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `SetNotify(bool) error` / `Notify() (bool, error)` | 활성화 시 peer connect/disconnect를 application 메시지로 전달 |

**Completion result.** 두 receive 형식 모두 `DontWait`에서 `(false, nil)` 관례를 따른다.

STREAM은 `Bind`로 들어오는 연결을 받는다. `Bind` 전에 RAW 또는 PACKET을 정하고
수신 모드에 맞는 메서드를 사용한다.

---

## 공유 flag와 enum

| 타입 | 사용처 | 값 |
|---|---|---|
| `SendFlags`(명명된 `int`) | synchronous publish `.Flags(...)` 단계 | `SendFlagsNone`, `SendFlagsDontWait` |
| `RecvFlags`(명명된 `int`) | 모든 `Recv`/`Subscribe`/`ReceiveSubscriptionEvent` | `RecvFlagsNone`, `RecvFlagsDontWait` |
| `RidDuplicatePolicy`(명명된 `int`) | `CommonSocketOptions.RidDuplicatePolicy`/`RouterSocket.SetHandover` | `RidDuplicateReject`, `RidDuplicateHandover` |
| `SubmitRetryMode`(명명된 `int`) | `CommonSocketOptions.SubmitRetryMode` | `SubmitRetryOff`, `SubmitRetryLocalFailure` |

**선택 기준.** 이들은 `String()` 메서드가 있는 Go `iota` enum이나
`[Flags]` 스타일 bitmask 타입이 아니라 package-level 상수를 가진 평범한
명명된 `int` 타입이다 — 이 binding에서 어느 것도 `fmt.Stringer`를
구현하지 않는다.

---

[`internal/native/socket_core.go`](../../../../bindings/go/internal/native/socket_core.go),
[`socket_types.go`](../../../../bindings/go/internal/native/socket_types.go),
[Go 바인딩 스펙](../../spec/go/README.ko.md)에서 전체 근거를 확인한다.
