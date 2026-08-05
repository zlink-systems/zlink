한국어 | [English](03-sockets.en.md)

[레퍼런스 목차](README.ko.md)

# 03. Sockets

이 category는 `CommonSocketOptions`/`PubSocketOptions`, 8개 concrete socket
타입, 그리고 Go struct embedding으로 얻는 공유 lifecycle 표면을 다룬다.
**socket type 전체를 아우르는 공유 interface는 없다** —
`PairSocket`/`PubSocket`/`SubSocket`/`DealerSocket`/`RouterSocket`/
`XPubSocket`/`XSubSocket` 각각은 export되지 않은 base(`directSocket`/
`publishSocket`/`subscribeSocket`/`routedSocket`)를 embed하고, 그 base는
다시 `connectionSocket`을 embed하고, 그건 다시 `socketCore`를 embed한다 —
`Bind`/`Connect`/`Unbind`/`Disconnect`/`DisconnectRID`/`Close`는 이 chain을
통해 `socketCore`로부터 자동으로 promote되며, 타입마다 재선언되지 않는다.
`StreamSocket`은 이 chain을 깬다: base를 embed 필드가 아니라 **명명된
field**(`core *routedSocket`)로 가지므로 아무것도 promote되지 않는다 —
노출하는 모든 메서드는 손으로 쓴 한 줄짜리 forward이며, 다른 모든 socket
type이 공짜로 얻는 여러 메서드(`Connect`, `Disconnect`, `DisconnectRID`,
`CommonOptions()`, 그리고 그로 인해 개별 forward되지 않은 모든
`CommonSocketOptions` accessor)가 `StreamSocket`엔 단순히 없다. 정확한
signature는
[`internal/native/socket_core.go`](../../../../bindings/go/internal/native/socket_core.go),
[`socket_types.go`](../../../../bindings/go/internal/native/socket_types.go),
[`socket_options.go`](../../../../bindings/go/internal/native/socket_options.go),
[`connection_socket.go`](../../../../bindings/go/internal/native/connection_socket.go),
그리고 언어별 파일(`socket_direct.go`, `socket_routed.go`,
`socket_publish.go`, `socket_subscribe.go`,
`socket_completion_control.go`)이 소유하며,
[`contracts/sockets.go`](../../../../bindings/go/contracts/sockets.go)를
통해 alias로 re-export된다. socket 생성 자체는 Core category에서
다룬다(`Context.PairSocket()` 등), 그리고 아래 모든 socket이 반환하는
`Send`/`Request`/`Reply` builder 계열은 Messaging category에서 다룬다.

---

## 공유 socket 표면(embedding을 통해, interface가 아니라)

**Options.** `StreamSocket`을 제외한 모든 socket type은 embedding을
통해 이 집합을 공짜로 얻는다. **결합형 TLS setter만 존재한다**; rust와
달리 개별 `SetTLSCert`/`SetTLSKey`/`SetTLSCA`/`SetTLSHostname` 메서드는
이 binding에 없다.

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

`StreamSocket`은 `Bind`, `Unbind`, `Close`, `LastEndpoint`,
`SetTLSServer`, `SetTLSClient`, 그리고 소수의 개별 `CommonSocketOptions`
값(`SetSendHighWaterMark`/`SendHighWaterMark`,
`SetReceiveHighWaterMark`/`ReceiveHighWaterMark`, `SetLinger`,
`SetReceiveTimeout`, `SetSendTimeout`, `SetTCPKeepalive`,
`SetTCPNoDelay`, `SetIPv6`)만 손으로 forward한다 — `Connect`,
`Disconnect`, `DisconnectRID`, `CommonOptions()`가 전혀 없고, 따라서
`Immediate`, `RidDuplicatePolicy`, `MaxMessageSize`, `Backlog`,
`ReconnectInterval`, `ConnectTimeout`, 다른 모든 socket type이 설정할
수 있는 `SubmitRetry*` 계열에도 닿을 방법이 없다.

**Completion result.** 이 메서드 전부 `error`(또는 getter의 경우 `(T,
error)`)를 반환한다.

**선택 기준.** `StreamSocket`의 더 좁은 표면을 우회해야 할 실수가 아니라
확고한 제약으로 취급한다 — forward되지 않은 option은 이 타입의 다른 어떤
공개 경로로도 닿을 수 없다.

---

## `CommonSocketOptions` / `PubSocketOptions`

typed option facade — `CommonSocketOptions`는 `connectionSocket`을
embed하는 모든 socket type이 공유하고(`.CommonOptions()`로 접근),
`PubSocketOptions`는 PUB/XPUB 전용이다(`.PubOptions()`로 접근).

```go
opts := dealer.CommonOptions()
opts.SetSendHighWaterMark(100_000)
opts.SetLinger(time.Second)
opts.SetSubmitRetryMode(contracts.SubmitRetryLocalFailure)
```

**Options — `CommonSocketOptions`.** 모든 accessor가 `(T, error)`/`error`를
반환한다.

| Member | 의미 |
| --- | --- |
| `Linger()` / `SetLinger(time.Duration)` | `Close()`가 대기 중인 send를 flush하기 위해 기다리는 상한 |
| `SendHighWaterMark()` / `SetSendHighWaterMark(int)` | outbound accounted-byte HWM |
| `ReceiveHighWaterMark()` / `SetReceiveHighWaterMark(int)` | inbound accounted-byte HWM |
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
pair.Send().Message(ping).Submit(ctx)
var received contracts.Received
ok, err := pair.Recv(&received, contracts.RecvFlagsNone)
```

**Options.** 위 공유 lifecycle/TLS/option 표면에 더해:

| Member | 의미 |
| --- | --- |
| `Send() SendOp` | 공유 send builder를 시작 |
| `Recv(out *Received, flags RecvFlags) (bool, error)` | `out`을 다음 메시지로 채움 |
| `OnSendReady(handler func()) error` | back-pressure 해제 콜백을 등록 |

**Completion result.** `Recv`는 `RecvFlagsDontWait`가 설정돼 있고
메시지가 없을 때만 `(false, nil)`을 반환한다.

**선택 기준.** 배타적 point-to-point 링크엔 PAIR를 쓴다 — peer routing이
없고 load-balance하지 않는다.

---

## `DealerSocket`

연결된 peer 전체에 send를 load-balance하고 routed request를 발행할 수
있다.

```go
dealer, err := ctx.DealerSocket()
dealer.SetRoutingID(contracts.NewRoutingIDString("worker-3"))
dealer.Request().Message(payload).Submit(ctx, func(result contracts.RequestResult, parts []*contracts.Message) {
    // ...
})
```

**Options.** `PairSocket`과 같은 공유 표면에 더해:

| Member | 의미 |
| --- | --- |
| `Send() SendOp` | 공유 send builder를 시작 |
| `Recv(out, flags) (bool, error)` | `out`을 다음 메시지로 채움 |
| `Request() RequestOp` | 공유 request builder를 시작; target 인자 없음 — DEALER엔 API 레벨 peer routing id가 없기 때문 |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `SetProbe(bool) error` | **set-only, getter 없음**; connect 시 빈 probe 전송 |
| `Weight()` / `SetWeight(int)` | load-balancing 가중치, 양방향 모두 |
| `SetRequestTimeout(time.Duration) error` | **set-only, getter 없음** — 다른 모든 언어의 Dealer 타입에서 나타나는 동일한 비대칭과 일치 |

**Completion result.** `Recv`는 `PairSocket`과 같은 `DontWait`에서
`(false, nil)` 관례를 따른다.

**선택 기준.** peer가 첫 메시지부터 관찰하도록 connect 전에
`SetRoutingID`를 설정한다. DEALER엔 임의 token에 응답하는 프로토콜
envelope helper가 없다 — 대신 수신 request context의
`Received.Reply()`를 쓰거나 ROUTER의 명시적 reply 표면을 쓴다.

---

## `RouterSocket`

routing id로 지정된 peer에 메시지를 보내고, 특정 peer의 request에
응답하며, 추가로 opaque completion-control channel을 지원한다.

```go
router, err := ctx.RouterSocket()
router.SendTo(peerRID).Message(hello).Submit(ctx)
router.OnCompletionControl(func(received *contracts.Received) {
    defer received.Close()
})
```

**Options.** `PairSocket`과 같은 공유 표면에 더해. **이 binding은
ROUTER에서 completion-control을 실제로 구현한다**, 대응하는 공개
진입점을 선언하지 않는 rust와 다르다.

| Member | 의미 |
| --- | --- |
| `SendTo(target RoutingID) SendOp` | 공유 send builder를 그 peer로 향해 시작 |
| `Recv(out, flags) (bool, error)` | `out`을 다음 메시지로 채움; receive-callback handler가 이미 설치돼 있으면 `*RecvError{Result: RecvBusy}`를 반환 — 이 binding에서 `Recv`와 callback 경로는 ROUTER에서 상호 배타적 |
| `Request(peerRID RoutingID) RequestOp` | Messaging category의 `RequestOp`, 특정 peer로 향함 |
| `Reply(rid RoutingID, requestSeq uint64) ReplyOp` | Messaging category의 `ReplyOp`, 해당 peer의 request에 응답 |
| `OnSendReady(handler func()) error` | back-pressure 해제 콜백을 등록 |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `SetMandatory(bool) error` | **set-only**; 알 수 없는 route에서 조용히 버리는 대신 오류 |
| `SetProbe(bool) error` | **set-only**; connect 시 빈 probe 전송 |
| `SetHandover(bool) error` | **set-only** — `CommonOptions().SetRidDuplicatePolicy` 위의 편의 메서드, `true`를 `RidDuplicateHandover`로 매핑 — 두 경로 모두 같은 밑바탕 option에 닿는다 |
| `SetConnectRoutingID(RoutingID) error` | **set-only — 할당된 connect routing id의 getter 없음**, 읽기 전용 `ConnectRoutingId` property를 가진 dotnet/cpp와 다름 |
| `Weight()` / `SetWeight(int)` | load-balancing 가중치, 양방향 모두 |
| `RequestTimeout()` / `SetRequestTimeout(time.Duration)` | request timeout, **양방향 모두, Dealer의 set-only와 다름** |
| `OnCompletionControl(handler func(*Received)) error` | 들어오는 completion-control record를 받는 콜백을 등록 |
| `CompletionControl(peerRID RoutingID) SendOp` | peer로 향하는 opaque control-plane record; control plane은 send flag를 전혀 받지 않는다 — `SendFlagsNone` 외의 `Flags` 값은 submit 시점에 거부된다 |

**Completion result.** `Recv`는 `PairSocket`과 같은 관례를 따른다.

**선택 기준.** DEALER가 특정 peer를 지정할 수 없는 곳에서
ROUTER-시작 또는 ROUTER-응답 request/reply엔
`Request(peerRID)`/`Reply(rid, requestSeq)`를 쓴다. 일반 application
payload와 혼동되면 안 되는 out-of-band control 메시지엔
`OnCompletionControl`/`CompletionControl`을 쓴다.

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
pubSocket.Publish("prices").Message(tick).Submit(ctx)

sub, err := ctx.SubSocket()
sub.SetSubscription("prices.")
var msg contracts.TopicMessage
ok, err := sub.Subscribe(&msg, contracts.RecvFlagsNone)
```

**Options.** **`PubSocket`도 `XPubSocket`도 `SetRoutingID`/`RoutingID`를
선언하지 않는다** — 둘 다 이 binding에 routing-id 표면이 전혀 없다,
지금까지 다룬 다른 모든 언어와 동일하다. **`SubSocket`도 `XSubSocket`도
`OnSendReady`를 선언하지 않는다** — 이 binding에서 유일하게
`OnSendReady`가 없는 socket type이다.

| 타입 | Member | 의미 |
| --- | --- | --- |
| `PubSocket` | `Publish(topic string) SendOp` | 공유 send builder를 시작 |
| | `OnSendReady(handler func()) error` | back-pressure 해제 콜백을 등록 |
| | `PubOptions() *PubSocketOptions` | 타입별 option facade |
| `SubSocket` / `XSubSocket` | `Subscribe(out *TopicMessage, flags RecvFlags) (bool, error)` | `out`을 다음 매칭 publish로 채움 |
| | `SetSubscription(filter string) error` / `UnsetSubscription(filter string) error` | topic filter를 추가/제거; subscription은 누적된다 |
| | `SubscriptionAt(index int) (string, bool, error)` | `(filter, isPattern, error)` triple — 해당 index의 filter |
| | `TopicsCount() (int, error)` | 활성 구독 개수 |
| `XPubSocket` | `ReceiveSubscriptionEvent(out *SubscriptionEvent, flags RecvFlags) (bool, error)` | `out`을 다음 subscribe/unsubscribe로 채움, `publishSocket`을 통해 embed하는 `PubSocket` 표면 위에 추가됨 |
| | `Publish(topic string) SendOp` | 자신만의 복사본, `PubSocket`에서 재사용된 게 아니지만 형태는 동일 |

**Completion result.** `Subscribe`/`ReceiveSubscriptionEvent`는 같은
`DontWait`에서 `(false, nil)` 관례를 따른다.

**선택 기준.** subscriber 이탈을 관찰하려고 특별히 `XPubSocket`을
`ReceiveSubscriptionEvent`와 함께 쓰거나, `PubSocketOptions.SetManual`/
`ApproveSubscribe`/`RejectSubscribe`로 수동 admission을 쓴다.
subscription을 일반 메시지로 실어야 할 때 특별히 `XSubSocket`을 쓴다.

---

## `StreamSocket`

다른 모든 socket type이 쓰는 zlink wire protocol 밖에서, raw TCP peer와
직접 framed packet을 주고받는다. base를 embedding이 아니라 명명된
field로 감싼다 — 이게 무엇을 빠뜨리는지는 이 category 맨 위 참고를
본다.

```go
stream, err := ctx.StreamSocket()
stream.OnPacket(func(routingID contracts.RoutingID, header, body *contracts.Message) {
    // header/body를 소유
})
```

**Options.** `StreamSocketOptions` facade 타입은 존재하지 않는다 —
`SetNotify`/`Notify`는 `StreamSocket`에 직접 선언돼 있다,
`PubSocketOptions`의 별도 facade 형태와 다르다.

| Member | 의미 |
| --- | --- |
| `SendTo(target RoutingID) SendOp` | 공유 send builder를 그 peer로 향해 시작 |
| `Recv(out *Received, flags RecvFlags) (bool, error)` | `out`을 다음 packet으로 채움; 이 binding의 `Recv`는 추가로 source routing id를 `out`에 send context로 캡처한다, 그래서 이어지는 `out.Send()`는 packet의 발신자로 향한다 — STREAM 전용 보강 |
| `OnPacket(handler func(RoutingID, *Message, *Message)) error` | callback 기반 packet loop를 등록; handler가 `header`와 `body` 둘 다 소유 |
| `OnSendReady(handler func()) error` | back-pressure 해제 콜백을 등록 |
| `SetRoutingID(RoutingID) error` / `RoutingID() (RoutingID, error)` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `SetNotify(bool)` / `Notify() (bool, error)` | 활성화 시 peer connect/disconnect를 application 메시지로 전달 |

**Completion result.** `Recv`는 `DontWait`에서 `(false, nil)` 관례를
따른다.

**선택 기준.** callback 기반 packet loop엔 `OnPacket`을 쓴다.
`StreamSocket`은 이 binding의 공개 API 관점에서 아예 `Connect`/
`Disconnect`가 불가능하다는 걸 기억한다 — bind-and-accept 전용이다.

---

## 공유 flag와 enum

| 타입 | 사용처 | 값 |
|---|---|---|
| `SendFlags`(명명된 `int`) | 모든 send/request/reply builder의 `.Flags(...)` 단계(Messaging category) | `SendFlagsNone`, `SendFlagsDontWait` |
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
