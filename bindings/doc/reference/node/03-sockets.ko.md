한국어 | [English](03-sockets.en.md)

[레퍼런스 목차](README.ko.md)

# 03. Sockets

이 category는 `Socket`/`ConnectableSocket`(공유 기반 interface),
`CommonSocketOptions`와 타입별 확장, 8개 구체 socket interface, 공유
상수를 다룬다. 모든 socket의 `send`/`publish`/`request`/`reply`는
Messaging category에 문서화된 operation-builder family를 반환한다 — 이
category는 각 builder가 어디서 시작하고 각 socket type이 고유하게 무엇을
더하는지만 다룬다. socket은 `Context`의 메서드가 아니라 최상위
`createXxxSocket(ctx)` 함수(Core category)로 생성된다. 정확한 signature는
[`contracts/sockets/`](../../../../bindings/node/src/zlink/contracts/sockets/)가
소유한다.

---

## `Socket` / `ConnectableSocket` 공유 기반

모든 socket type이 확장하는 기반 interface: binding, TLS, monitoring,
disposal, (그리고 `StreamSocket`을 제외한 모든 socket type의) outbound
connection.

```ts
socket.bind('tcp://*:5555');
socket.setTlsServer(certPath, keyPath, true);
const monitor = socket.monitorOpen(SOCKET_MONITOR_EVENT_ALL);
socket.close();
```

**Options.** 아래 모든 구체 socket type은 **`StreamSocket`을 제외하고**
`ConnectableSocket`을 확장한다 — `StreamSocket`은 `Socket`을 직접
확장하고 자신의 `disconnectRid`를 독립적으로 선언한다(아래 참고).
`BaseSocket` union 타입(`PairSocket | PubSocket | SubSocket |
DealerSocket | RouterSocket | XPubSocket | XSubSocket | StreamSocket`)은
`proxy`/`proxySteerable`(Core category)처럼 임의 구체 socket type을
받는 API를 위해 export된다.

| Member | 의미 |
| --- | --- |
| `bind(endpoint: string)` / `unbind(endpoint: string)` | 주소에서 listen을 시작/중단 |
| `close()` | native socket을 닫는다 |
| `monitorOpen(events?: number, handler?: SocketMonitorHandler)` | monitor를 연다; 지금까지 다룬 다른 모든 언어와 달리 **handler를 open 시점에 이 메서드의 두 번째 인자로 직접 등록할 수 있다**, 항상 이후에 별도의 `onEvent` 스타일 호출이 필요한 게 아니라 |
| `setTlsServer(cert, key, requireClientCert?)` | `bind` 전에 적용 |
| `setTlsClient(ca, hostname, trustSystem?)` | `connect` 전에 적용 |
| `ConnectableSocket.connect(endpoint: string)` / `disconnect(endpoint: string)` | peer 주소로 connect/disconnect |
| `ConnectableSocket.disconnectRid(routingId: RoutingId)` | 해당 routing id로 식별되는 peer를 disconnect |

**Completion result.** `monitorOpen`을 제외한 모든 member는 반환값 없이
동기다. `monitorOpen`은 `MonitorSocket`(Eventing category)을 동기로
반환한다 — caller가 소유하며 반드시 close해야 한다.

**선택 기준.** `bind`/`connect` 전에 각각 `setTlsServer`/`setTlsClient`를
호출한다. `recv` 스타일 drain을 위해 monitor를 따로 유지할 필요가 없을
땐 handler를 `monitorOpen(events, handler)`에 직접 넘긴다.

---

## `CommonSocketOptions`와 타입별 확장

`socket.options`로 도달하는, 모든 socket type이 공유하는 typed option
facade. getter/setter 메서드 쌍이 아니라 일반 property 접근으로
get/set하는 mutable property의 순수 interface다.

```ts
socket.options.sendHwm = 100_000n;
socket.options.linger = 1000;
socket.options.submitRetryMode = SubmitRetryMode.LocalFailure;
```

**Options — `CommonSocketOptions`.**

| Member | 타입 | 의미 |
| --- | --- | --- |
| `linger` | `number`, ms | `close()`가 대기 중인 send를 flush하기 위해 기다리는 상한 |
| `sendHwm` / `recvHwm` | `bigint`, accounted-byte HWM | outbound/inbound HWM; `0n`은 무제한 |
| `sendTimeout` / `recvTimeout` / `connectTimeout` | `number`, ms | blocking send/receive/connect handshake가 기다리는 상한 |
| `immediate` | `boolean` | send가 지금 당장 살아있는 연결을 요구하는지, 아니면 연결이 생길 때까지 큐잉하는지 |
| `ridDuplicatePolicy` | `RidDuplicatePolicyValue` | peer가 기존 routing id를 재사용할 때 벌어지는 일 |
| `ipv6` | `boolean` | socket이 IPv6 연결을 받아들이는지 |
| `tcpNoDelay` | `boolean` | `true`면 Nagle 알고리즘을 비활성화 |
| `tcpKeepalive` | `number`, -1/0/1 | OS TCP keepalive 모드 |
| `maxMsgSize` | `bigint` | 수신 허용하는 메시지 한 건의 최대 바이트 크기; `-1n`은 제한 없음 |
| `lastEndpoint` | `string`, 읽기 전용 | 실제로 해석된 bind 주소 |
| `backlog` | `number` | listening socket의 대기 연결 큐 길이 |
| `reconnectInterval` / `reconnectIntervalMax` | `number`, ms | 재연결 시도 사이 간격 / 그 간격의 상한 |
| `submitRetryMode` | **순수 `number`로 타입 지정, `SubmitRetryModeValue`가 아님** — `ridDuplicatePolicy`의 typed property와 비교하면 비일관적 | local back-pressure에서 실패한 submit이 자동 재시도되는지 |
| `submitRetryTimeout` | `number`, ms | `submitRetryMode`가 요청할 때의 재시도 timeout |
| `submitRetryAttempts` | `number` | `submitRetryMode`가 요청할 때의 재시도 횟수 상한 |

**Options — 타입별 확장.**

| 타입 | Member | 의미 |
| --- | --- | --- |
| `DealerSocketOptions` | `probe`(`boolean`) | connect 시 빈 probe 전송 |
| | `requestTimeout`(`number`, ms) | request timeout |
| | `peerWeight`(`number`, 0-100) | load-balancing 가중치 |
| `RouterSocketOptions` | `mandatory`(`boolean`) | 알 수 없는 route에서 조용히 버리는 대신 오류 |
| | `handover`(`boolean`) | `ridDuplicatePolicy`의 편의 wrapper |
| | `probe`(`boolean`) | connect 시 빈 probe 전송 |
| | `connectRoutingId`(`RoutingId \| null`, 읽기 전용) / `setConnectRoutingId(routingId)` | getter·setter 이름이 비대칭 |
| | `requestTimeout` / `peerWeight` | Dealer와 같은 형태, 양방향 모두 |
| `StreamSocketOptions` | `notify`(`boolean`) | 활성화 시 peer connect/disconnect를 application 메시지로 전달 |
| `PubSocketOptions` | `verbose` / `verboser`(`boolean`) | 중복 포함 모든 (un)subscribe 메시지를 전달 |
| | `noDrop`(`boolean`) | back-pressure에서 조용히 버리는 대신 오류 |
| | `manual` / `manualLastValue`(`boolean`) | 구독이 `approveSubscribe`/`rejectSubscribe`를 요구; `manualLastValue`는 새로 승인된 구독자에게 topic별 마지막 캐시 메시지도 재전송 |
| | `topicsCount`(`number`, 읽기 전용) | 활성 구독 개수 |
| | `welcomeMessage()` / `setWelcomeMessage(message: MessageLike)` | 새로 연결된 구독자 각각에게 자동 전송; getter는 caller가 소유하는 `Message`를 반환 |
| | `approveSubscribe(routingId)` / `rejectSubscribe(routingId)` | `manual` 필요; set-only, getter 없음 |
| `SubSocketOptions` | `topicsCount`(읽기 전용) | 이 socket이 가진 유일한 타입별 option |

**Completion result.** 모든 property 읽기/쓰기는 동기다.

**선택 기준.** 기본값이 배포 환경에 맞지 않을 때 socket이 메시지 교환을
시작하기 전에 `sendHwm`/`recvHwm`, `linger`를 설정한다. wildcard 주소로
bind한 후 `lastEndpoint`를 읽는다.

---

## `PairSocket`

라우팅이 없는 배타적 1:1 peering socket. `DealerSocket`이 확장하는
기반 형태 역할을 한다(아래 참고) — Pair와 Dealer에 별도 공유
`IMessageSocket` 스타일 기반을 주는 dotnet/java/cpp와 달리, 여기선
`DealerSocket extends PairSocket`이 직접 확장한다.

```ts
const pair = createPairSocket(ctx);
pair.send().message(Message.from('ping')).submit();
const received = new Received();
if (pair.recv(received)) { /* ... */ }
```

**Options.**

| Member | 의미 |
| --- | --- |
| `options` | `CommonSocketOptions` |
| `send()` | 공유 `SendOperation` builder를 시작 |
| `recv(result: Received, flags?: RecvFlags)` | `result`를 다음 메시지로 채움 |
| `setSendReadyHandler(handler: () => void)` | back-pressure 해제 콜백을 등록 |

**Completion result.** `recv`는 `boolean`을 반환한다 —
`RecvFlags.DontWait`가 설정되고 메시지가 없을 때만 `false`다.

**선택 기준.** 배타적 point-to-point 링크엔 PAIR를 쓴다 — peer 라우팅이
없고 load-balance하지 않는다.

---

## `DealerSocket`

연결된 peer 전체에 send를 load-balance하고 routed request를 낼 수
있다. `PairSocket`을 직접 확장한다(별도 공유 message-socket interface가
아니라).

```ts
const dealer = createDealerSocket(ctx);
dealer.setRoutingId(RoutingId.from('worker-3'));
const reply = await dealer.request().message(Message.from('payload')).submit();
```

**Options.** `PairSocket`의 모든 것에 더해:

| Member | 의미 |
| --- | --- |
| `options` | `DealerSocketOptions`로 override됨 |
| `setRoutingId(routingId)` / `getRoutingId()` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `request()` | 공유 `RequestOperation` builder를 시작; target 인자 없음 — DEALER는 API 레벨 peer routing id가 없기 때문 |

**Completion result.** (상속된) `recv`는 `PairSocket`과 같은 `boolean`
관례를 따른다.

**선택 기준.** peer가 첫 메시지부터 이를 관찰하도록 connect 전에
`setRoutingId`를 설정한다. DEALER는 임의 token에 reply할 protocol
envelope helper가 없다 — 대신 수신된 request context
(`Received.reply()`)나 명시적 ROUTER reply 표면에서 답한다.

---

## `RouterSocket`

routing id로 지정된 peer에게 메시지를 보내고, 특정 peer의 request에
reply할 수 있다. `PairSocket`이 아니라 `ConnectableSocket`을 직접
확장한다.

```ts
const router = createRouterSocket(ctx);
router.send(peerRid).message(Message.from('hello')).submit();
router.setCompletionControlHandler((rid, parts) => { /* ... */ });
```

**Options.**

| Member | 의미 |
| --- | --- |
| `options` | `RouterSocketOptions` |
| `send(routingId)` | 공유 `SendOperation`을 그 peer로 향해 시작 |
| `recv(result: Received, flags?: RecvFlags)` | `result`를 다음 메시지로 채움 |
| `setSendReadyHandler(handler)` | back-pressure 해제 콜백을 등록 |
| `setRoutingId(routingId)` / `getRoutingId()` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `request(peerRid)` | Messaging category의 `RequestOperation`, 특정 peer로 향함 |
| `reply(peerRid, requestSeq: bigint)` | Messaging category의 `ReplyOperation`, 해당 peer의 request에 응답 |
| `trySendCompletionControl(peerRid, parts: readonly MessageLike[])` | peer의 기존 연결로 opaque control record를 전송; `parts`를 소비하지 않음 |
| `setCompletionControlHandler(handler: (sourceRoutingId, parts: Message[]) => void)` | 들어오는 completion-control record를 받는 콜백을 등록; handler가 수신 메시지를 소유 |

**Completion result.** `trySendCompletionControl`은 `boolean`을
반환한다 — completion connection이 back-pressure일 때만 `false`다.
`recv`는 위 `boolean` 관례를 따른다.

**선택 기준.** DEALER가 특정 peer를 지정할 수 없는 ROUTER 주도·ROUTER
응답 request/reply엔 `request(peerRid)`/`reply(peerRid, requestSeq)`를
쓴다. application-level receive와 독립적인 opaque bounded control
record엔 `trySendCompletionControl`/`setCompletionControlHandler`를
쓴다.

---

## `PubSocket` / `XPubSocket`

PUB는 매칭되는 구독자가 없으면 버리는 topic-filtered 메시지를
publish하고, XPUB는 추가로 구독자의 subscribe/unsubscribe event를
노출한다. `XPubSocket extends PubSocket`.

```ts
const pub = createPubSocket(ctx);
pub.publish('prices').message(Message.from(tick)).submit();

const xpub = createXPubSocket(ctx);
const evt = new SubscriptionEvent();
if (xpub.receiveSubscriptionEvent(evt)) { /* ... */ }
```

**Options.** `PubSocket extends ConnectableSocket`; `XPubSocket extends PubSocket`.

| Member | 의미 |
| --- | --- |
| `options` | `PubSocketOptions` |
| `publish(topic: string)` | 공유 `SendOperation` builder를 시작 |
| `setSendReadyHandler(handler)` | back-pressure 해제 콜백을 등록 |
| `receiveSubscriptionEvent(result: SubscriptionEvent, flags?: RecvFlags)` | `XPubSocket`에만 있음; `result`를 다음 subscribe/unsubscribe로 채움 |

**`PubSocket`엔 `setRoutingId`/`getRoutingId`가 없다**(둘 다 있는 dotnet의
`IPubSocket`과 다름).

**Completion result.** `receiveSubscriptionEvent`는 위와 같은 관례로
`boolean`을 반환한다.

**선택 기준.** `receiveSubscriptionEvent`로 구독자 변동을 관찰하거나
`PubSocketOptions.manual`/`approveSubscribe`/`rejectSubscribe`로 수동
admission을 하려면 특별히 `XPubSocket`을 쓴다. 그 외엔 publish 자체는
둘이 같게 동작한다.

---

## `SubSocket` / `XSubSocket`

SUB는 구독을 socket option으로 설정하는 방식으로 topic을 구독하고,
XSUB는 대신 구독을 메시지로 실어 나른다. `XSubSocket extends SubSocket
{}` — 완전히 빈 interface 본문으로, 지금까지 다룬 모든 wrapper binding
중 가장 순수한 delta-only 선언이다.

```ts
const sub = createSubSocket(ctx);
sub.setSubscription('prices.');
const msg = new TopicMessage();
if (sub.subscribe(msg)) { /* ... */ }
```

**Options.** `SubSocket extends ConnectableSocket`.

| Member | 의미 |
| --- | --- |
| `options` | `SubSocketOptions` |
| `setSubscription(filter: string)` / `unsetSubscription(filter: string)` | topic filter를 추가/제거; 구독은 누적된다 |
| `subscriptionAt(index: number)` | `SubscriptionEntry \| null` — 해당 index의 filter |
| `subscribe(result: TopicMessage, flags?: RecvFlags)` | `result`를 다음 매칭 publish로 채움 |

`XSubSocket`은 아무것도 더하지 않는다 — 모든 member가 상속된 `SubSocket`
표면 그대로다.

**Completion result.** `subscribe`는 위와 같은 관례로 `boolean`을
반환한다.

**선택 기준.** 일반적인 경우엔 `SubSocket`을 쓴다. 구독을 일반 메시지로
실어 날라야 할 때만 특별히 `XSubSocket`을 쓴다 — interface 자체는 둘을
구분할 게 없으므로 선택은 전적으로 어떤 구체 타입을 생성하는지
(`createSubSocket` vs `createXSubSocket`)에 달려 있다.

---

## `StreamSocket`

다른 모든 socket type이 쓰는 zlink wire protocol 밖에서, raw TCP peer와
framed packet을 직접 주고받는다. (`ConnectableSocket`이 아니라)
`Socket`을 확장하고 자신의 `disconnectRid`를 독립적으로 선언한다.

```ts
const stream = createStreamSocket(ctx);
stream.setPacketHandler((sourceRid, header, body) => { /* header/body 소유 */ });
```

**Options.**

| Member | 의미 |
| --- | --- |
| `options` | `StreamSocketOptions` |
| `send(routingId)` | 공유 `SendOperation`을 그 peer로 향해 시작 |
| `recv(result: Received, flags?: RecvFlags)` | `result`를 다음 packet으로 채움 |
| `setPacketHandler(handler: StreamPacketHandler)` | callback 기반 packet loop를 등록 |
| `setSendReadyHandler(handler)` | back-pressure 해제 콜백을 등록 |
| `setRoutingId(routingId)` / `getRoutingId()` | 이 socket 자신의 routing id를 지정/조회, peer가 connect 시 관찰 |
| `disconnectRid(routingId)` | `StreamSocket`이 `ConnectableSocket`의 사본을 상속하지 않으므로 이 interface에 직접 선언됨 |

**Completion result.** `recv`는 위 `boolean` 관례를 따른다. packet
handler는 수신하는 `header`와 `body` 메시지를 둘 다 소유한다.

**선택 기준.** callback 기반 packet loop엔 `setPacketHandler`를 쓴다.

---

## Socket 상수

위 모든 항목에서 참조하는 공유 상수 객체와 그 파생 타입.

| 상수 | 사용처 | 값 |
|---|---|---|
| `SocketType` | 내부 socket 종류 식별 | **이중 케이싱**: `ANY`/`PAIR`/`PUB`/`SUB`/`DEALER`/`ROUTER`/`XPUB`/`XSUB`/`STREAM`과 `Any`/`Pair`/`Pub`/`Sub`/`Dealer`/`Router`/`XPub`/`XSub`/`Stream` 둘 다 동일한 숫자값의 alias로 export됨 |
| `SOCKET_MONITOR_EVENT_ALL` | `Socket.monitorOpen(events)` | `0xFFFF` — "전체 구독" 편의 상수; 개별 lifecycle event flag(`Connected`, `Disconnected` 등)는 여기 `socket_constants.ts`가 아니라 Eventing category의 `contracts/eventing/monitor.ts`에 선언된 `MonitorEventType` 상수 객체다 |
| `RidDuplicatePolicy` | `CommonSocketOptions.ridDuplicatePolicy`, `RouterSocketOptions.handover` | `Reject`, `Handover` |
| `SubmitRetryMode` | `CommonSocketOptions.submitRetryMode`(property 자체는 순수 `number`로 타입 지정) | `Off`, `LocalFailure` |
| `SendFlags` | 모든 send/request/reply builder의 `.flags(...)` 단계(Messaging category) | `None`, `DontWait` |
| `RecvFlags` | 모든 `recv`/`subscribe`/`receiveSubscriptionEvent` | `None`, `DontWait` |
| `PollEventFlag` | Poller 등록/wait(Eventing category) | `PollIn`, `PollOut`, `PollErr`, `PollPri`, `PollCompletion` |

**선택 기준.** 두 flags 상수 어느 쪽이든 `DontWait`는 blocking 호출을
non-blocking으로 바꿔 block하는 대신 `false`/back-pressure를
보고한다. 모든 lifecycle event를 구독하려면 `monitorOpen(events)`에
`SOCKET_MONITOR_EVENT_ALL`을 넘기고, 구독을 필터링하려면 특정
`MonitorEventType` 값(Eventing category)을 OR해서 raw 숫자 mask로
넘긴다.

---

[`contracts/sockets/`](../../../../bindings/node/src/zlink/contracts/sockets/)와
[Node 바인딩 스펙](../../spec/node/README.ko.md)에서 전체 근거를 확인한다.
