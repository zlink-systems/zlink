# 03. Messaging execution — Channel messaging

[레퍼런스 목차](README.ko.md)

이 category는 `ZLinkRouteClient`(`ZLINK_ROUTE_CLIENT`)와 `ZLinkFanoutClient`(`ZLINK_FANOUT_CLIENT`)가
제공하는 진입점을 다룬다. 정확한 signature는
[Channel, request와 routing exact interface](../../common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md)가
소유한다. 이 문서는 그 signature를 반복하지 않고, 각 진입점을 실제로 호출할 때 필요한 정보만
완결된 형태로 모은다.

Framework는 같은 의미의 `ZLinkChannelClient`(ChannelName 전용, `sendToChannel`/`requestToChannel`만)도
DI로 제공한다. 이 문서는 `.NET`의 `IZLinkRouteClient`와 가장 가까운 `ZLinkRouteClient`를 기준으로
서술한다.

---

## `sendToChannel`

ChannelName에 등록된 ready target(RouteMesh 또는 ClientServer) 하나에 one-way message를 보낸다.
Reply를 기다리지 않는다.

```ts
await routeClient
  .sendToChannel("game.api", new PlayerOnline("player-1"))
  .submit();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` / `.metadata(metadata)` | 없음 | handler에 전달할 key-value |
| `.submit(signal?)` | 필수 terminal | source-local admission 성공까지만 기다린다 |

**완료 결과.** 정상 완료는 이 프로세스가 message를 큐에 수락했다는 뜻이다. Remote handler 실행이나
subscriber 수신은 기다리지 않는다. 큐 여유가 없으면 socket send timeout(공개 설정이 없으면 1초)까지
기다린 뒤 그래도 없으면 `DeadlineExceeded`로 reject한다. ChannelName에 ready target이 없으면
`NotFound`, route 단절은 `Unavailable`, runtime 종료 중이면 `ShuttingDown`으로 reject한다.

**선택 기준.** Reply가 필요 없는 fire-and-forget에 쓴다. Reply가 필요하면 `requestToChannel`을
쓴다.

---

## `requestToChannel`

ChannelName 하나로 ready target을 선택해 typed request를 보내고 typed reply를 기다린다.

```ts
const reply = await routeClient
  .requestToChannel("game.api", new GetPlayer("player-1"))
  .timeout(3_000)
  .submit<Player>();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` / `.metadata(metadata)` | 없음 | request에만 붙는다. reply는 request metadata를 자동 복사하지 않는다 |
| `.timeout(timeoutMs)` | MeshNode의 request 기본 timeout | reply를 기다리는 상한. 전송 admission 자체는 socket send timeout이 별도로 담당한다 |
| `.submit<TReply>(signal?)` | terminal(택 1) | reply 수신까지 기다린다 |
| `.yield<TReply>(signal?)` | terminal(택 1) | `SpotWide` User Spot·Instance Spot handler 안에서만 유효하다. 대기 동안 shared turn을 반환해 형제 job의 실행을 허용한다. 그 밖의 실행 context에서 호출하면 `invalidConfiguration`으로 완료한다 |

**완료 결과.** `TReply`(handler 반환값)로 완료하거나, timeout이면 `DeadlineExceeded`, ChannelName에
ready target이 없으면 `NotFound`, route 단절은 `Unavailable`, runtime 종료 중이면
`ShuttingDown`으로 reject한다.

**선택 기준.** Reply 값이 필요할 때 쓴다. One-way면 `sendToChannel`을 쓴다. `yield`는 `SpotWide`
handler 안에서 다른 request나 worker가 진행 중일 때, 자신의 대기가 형제 job을 막지 않게 하려고
쓴다.

---

## `sendToNode`

MeshName과 target Node RID를 직접 지정해 one-way message를 보낸다. ChannelName 기반 선택이 아니라
특정 MeshNode 하나를 관리할 때 쓴다.

```ts
await routeClient
  .sendToNode("play", "play-node-1", new DrainRequested())
  .submit();
```

**옵션.** `sendToChannel`과 동일하다 — `.metadata(...)`, terminal `.submit(signal?)`.

**완료 결과.** `sendToChannel`과 같은 완료 kind를 쓴다. 대상 RID가 Object Client(handler 등록이
불가능한 RID)이면 다른 target으로 넘기지 않고 `NotFound`로 완료한다.

**선택 기준.** 업무 object(actor·spot)의 배치나 메시징에는 쓰지 않는다 — 그 경우에는
ActorId·SpotId·ChannelName을 쓴다. Node direct는 운영 목적으로 특정 node를 지목할 때만 쓴다.

---

## `requestToNode`

MeshName과 target Node RID를 직접 지정해 typed request/reply를 주고받는다.

```ts
const status = await routeClient
  .requestToNode("play", "play-node-1", new GetNodeStatus())
  .submit<NodeStatus>();
```

**옵션.** `requestToChannel`과 동일하다 — `.metadata(...)`, `.timeout(...)`, terminal
`.submit<TReply>(signal?)` 또는 `.yield<TReply>(signal?)`.

**완료 결과와 선택 기준.** `requestToChannel`과 같되, 대상 선택이 ChannelName round-robin이 아니라
지정한 RID 고정이라는 점만 다르다.

---

## `publish` (classic fanout)

독립 fanout channel에 typed event를 발행한다. `ZLinkRouteClient`의 channel operation과는 다른
family다 — 발행자는 구독자를 알지 못한다.

```ts
await fanoutClient
  .publish("lobby.events", new PlayerJoined("player-1"))
  .submit();

// topic을 명시해야 하는 경우
await fanoutClient
  .publish("lobby.events", "region.eu", new PlayerJoined("player-1"))
  .submit();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| topic 인자 생략 | event의 packet name을 topic으로 사용 | 예약된 topic 이름(내부 liveness용 exact byte `01 5A 4C 46 31`)을 쓰면 `ZLinkConfigurationException`으로 완료한다 |
| `.submit(signal?)` | 필수 terminal | source-local publish admission 완료까지만 기다린다 |

**완료 결과.** 정상 완료는 발행 admission이 끝났다는 뜻이다. Subscriber 수나 수신 완료는 반환하지
않는다 — target이 0개여도 정상 완료한다. 시작한 뒤에는 개별 target 실패를 전체 실패로 바꾸지 않고
재시도하지 않는다.

**선택 기준.** 발행자가 구독자를 알지 못해야 하는 관찰·통지에 쓴다. 특정 대상에 보내는 메시징이면
`sendToChannel`이나 `requestToChannel`을 쓴다. Publisher listener의 advertised endpoint를
확인하려면 `ZLinkFanoutClient.getListenerStatus(channelName)`(topology-discovery category)를
쓴다.

---

## Codec 등록 (구성 시점)

다른 항목과 달리 terminal이 아니라 host 구성 시점의 등록 호출이다. JSON만 쓰는 application은 이
항목을 쓸 필요가 없다.

```ts
zlinkFramework().codecs().use(zlinkMessagePackCodec());
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.use(extension: ZLinkCodecExtension)` | 없으면 JSON | Business payload serializer를 등록한다. 여러 번 호출해 여러 content type을 등록할 수 있다 |

**완료 결과.** 반환값 없이 동기로 등록된다. Host 시작 전에만 호출한다.

**선택 기준.** JSON이 아닌 content type(MessagePack, Protobuf 등)을 쓸 때 쓴다. 공식 codec
extension 외에 `ZLinkCodecExtension`을 직접 구현해 custom serializer를 등록할 수도 있다.

STREAM 연결의 wire codec은 별도 계약(`ZLinkStreamCompressionBuilder`, topology-discovery
category의 "기타 host-wide 옵션" 항목)이다. 이 항목은 business payload serializer만 다룬다.

---

## 공통 실패·취소 규칙 (모든 항목에 적용)

이 category의 모든 진입점에 공통으로 적용되며, 항목마다 반복하지 않는다.

- `AbortSignal`이 `submit(...)` 전에 이미 abort 상태이면 runtime admission을 시작하지 않고
  `AbortError`로 reject한다.
- Admission이 시작된 뒤에는 abort, timeout, shutdown과 수락 중 먼저 확정된 terminal 결과만 남기며,
  abort나 timeout 뒤에 같은 operation을 다시 제출하지 않는다.
- 잘못된 argument·handle·state와 중복 submit은 exceptional completion(Promise rejection)으로
  처리하며, 이 문서가 나열하는 완료 kind(`NotFound`/`Unavailable`/`DeadlineExceeded`/
  `ShuttingDown`)와는 다른 층이다.

전체 근거는
[Channel, request와 routing exact interface](../../common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md)와
[기초 타입과 구성 exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md)를 참고한다.
