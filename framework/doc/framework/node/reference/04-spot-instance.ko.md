# 04. Spot instance

[레퍼런스 목차](README.ko.md)

이 category는 `ZLinkSpotManager`(`ZLINK_SPOT_MANAGER`)·`ZLinkRouteClient`·`ZLinkSpotPublisherClient`
(`ZLINK_SPOT_PUBLISHER_CLIENT`)가 제공하는 외부 진입점과, Spot 코드 안에서 `ZLinkSpotContext`/
`ZLinkInstanceSpotContext`로 쓰는 진입점을 다룬다. 정확한 signature는
[Spot과 Instance Spot exact interface](../../common/spec/server/languages/node/interfaces/04-spots.ko.md)와
[STREAM, timer와 worker exact interface](../../common/spec/server/languages/node/interfaces/06-stream-worker.ko.md)가
소유한다.

---

## `ZLinkSpotManager.create`

새 User Spot을 항상 새로 만든다. Framework가 새 global SpotId를 발급한다.

```ts
const created = await spotManager
  .create("room")
  .inMesh("play")
  .request(new CreateRoom("ranked"))
  .timeout(5_000)
  .submit();

const spotId = created.spot.spotId;
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.inMesh(meshName)` | Object Client·Server role의 Mesh가 하나면 생략 가능 | Spot을 생성할 Mesh. 후보가 둘 이상인데 생략하면 `InvalidOperation`, 없으면 `NotConfigured`, 지정한 Mesh가 없으면 `NotFound` |
| `.request(request)` | 없음(빈 요청) | Spot의 `onCreate(...)`에 전달할 생성 요청 |
| `.timeout(timeoutMs)` | resolve·factory·initialize 전체에 적용되는 기본값 | 생성 전체가 terminal state가 될 때까지의 상한 |
| `.submit(signal?)` | terminal(택 1) | 생성 완료까지 기다린다 |
| `.yield(signal?)` | terminal(택 1) | `SpotWide` handler 안에서만 유효 |

**완료 결과.** `ZLinkSpotCreateResult.state`가 `"created"`(새로 생성)다. Spot의 `onCreate(...)`가
거부하면 `"rejected"`이고 `reply`에 거부 메시지가 담긴다. 같은 option을 두 번 설정하거나 terminal을
두 번 호출하면 `InvalidOperation`, deadline 안에 끝나지 않으면 `DeadlineExceeded`다.

**선택 기준.** 항상 새 인스턴스가 필요할 때 쓴다. 있으면 재사용하고 없을 때만 만들려면
`getOrCreate`를 쓴다.

---

## `ZLinkSpotManager.getOrCreate`

지정한 SpotId의 Ready Spot이 있으면 그것을 반환하고, 없으면 새로 만든다.

```ts
const existingOrCreated = await spotManager
  .getOrCreate("lobby-eu", "lobby")
  .inMesh("play")
  .request(new CreateLobby("eu"))
  .submit();
```

**옵션.** `create`와 동일하다 — `.inMesh(...)`, `.request(...)`, `.timeout(...)`, terminal
`.submit(signal?)` 또는 `.yield(signal?)`.

**완료 결과.** `state`가 `"existing"`이면 이미 있던 Spot을 그대로 반환하고 `request`는 무시한다.
`"created"`면 새로 만든 것이다. 같은 SpotId가 creating 상태로 경합 중이면 그 결과를 기다렸다가
합류하고, cleanup으로 missing이 되면 새 reservation을 다시 경쟁한다.

**선택 기준.** SpotId로 멱등하게 "있으면 쓰고 없으면 만들기"가 필요할 때 쓴다. 항상 새 인스턴스가
필요하면 `create`를 쓴다.

---

## `find` / `close` (manager)

기존 Spot을 조회하거나 정확한 incarnation을 닫는다.

```ts
const spot = await spotManager.find("lobby-eu");

if (spot) {
  const closed = await spotManager.close(spot);
}
```

**옵션.** 두 호출 모두 modifier가 없다 — 대상 식별자만 받는다.

**완료 결과.** `find`는 Ready Spot이 없으면 `undefined`를 반환한다. `close`는 해당 incarnation이
없으면 `false`, generation이 다르면 `InvalidOperation`, pre-commit seal 중이면 `Unavailable`이다.
User Spot에 Actor membership이 남아 있으면 `false`이며 Actor를 자동으로 leave·destroy하지 않는다.

**선택 기준.** 지금 시점의 존재 여부 확인이나 명시적 종료가 필요할 때 쓴다. `close`는 stale
`SpotRef`로 다른 incarnation을 대신 닫지 않는다.

---

## `sendToSpot`

Global SpotId 하나로 one-way message를 보낸다. 외부 client(`ZLinkRouteClient`)와 Spot 코드 안
(`ZLinkSpotOutbound`)이 같은 모양을 제공한다.

```ts
await routeClient
  .sendToSpot("room-42", new PlayerJoinedRoom("player-1"))
  .submit();

// Instance Spot을 필요하면 새로 활성화(cold activation)해서 보내는 경우
await routeClient
  .sendToSpot("device-42", new DeviceCommand("reboot"))
  .instanceSpot("device")
  .submit();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.metadata(key, value)` | 없음 | handler에 전달할 key-value |
| `.instanceSpot()` | 없음(User Spot만 resolve) | Missing이면 cold activation한다. Existing authority가 있으면 등록 타입 수와 관계없이 저장된 stable type을 사용한다 |
| `.instanceSpot(instanceSpotType)` | — | Missing인데 등록 타입이 여럿이면 stable type을 명시해야 한다 |
| `.inMesh(meshName)` | Object Client·Server role의 Mesh가 하나면 생략 가능 | Missing Instance Spot을 처음 만들 Mesh. Instance marker 없이 쓰면 `InvalidOperation` |
| `.submit(signal?)` | 필수 terminal | source-local admission까지만 기다린다 |

**완료 결과.** SpotId가 없고 Instance marker도 없으면 `NotFound`. `.instanceSpot(...)`을 썼는데
existing authority가 User Spot이거나 명시한 타입과 다르면 `TypeMismatch`. 그 외 완료 kind는
messaging-execution category의 공통 규칙과 같다.

**선택 기준.** Reply가 필요 없는 Spot 메시징에 쓴다. Reply가 필요하면 `requestToSpot`을 쓴다.

---

## `requestToSpot`

Global SpotId 하나로 typed request/reply를 주고받는다.

```ts
const reply = await routeClient
  .requestToSpot("room-42", new GetRoomState())
  .timeout(3_000)
  .submit<RoomState>();
```

**옵션.** `sendToSpot`과 동일한 `.instanceSpot(...)`/`.inMesh(...)`에 더해 다음이 있다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.timeout(timeoutMs)` | MeshNode의 request 기본 timeout | resolve, cold activation, handler, reply 전체의 deadline |
| `.submit<TReply>(signal?)` | terminal(택 1) | reply 수신까지 기다린다 |
| `.yield<TReply>(signal?)` | terminal(택 1) | `SpotWide` User Spot·Instance Spot handler 안에서만 유효. 그 밖에서 호출하면 `invalidConfiguration`으로 완료 |

**완료 결과.** `sendToSpot`과 같은 실패 kind에 더해, cold activation 중 factory나 initialize가
실패하면 typed failure로 완료된다 — Framework가 내부적으로 재시도하지 않는다.

**선택 기준.** Reply 값이 필요할 때 쓴다. One-way면 `sendToSpot`을 쓴다.

---

## `publish` (Spot Logical Multicast)

ChannelName과 topic으로 구독자에게 typed event를 발행한다. `ZLinkSpotPublisherClient`(외부)와
`ZLinkSpotOutbound`(Spot 코드 안)가 같은 모양을 제공한다.

```ts
await spotPublisherClient
  .publish("room.events", "room-42", new RoomStateChanged("started"))
  .submit();
```

**옵션.** 이 호출에는 `.metadata(...)`와 필수 terminal `.submit(signal?)`이 있다 — topic은 필수
인자다.

**완료 결과.** 정상 완료는 발행 admission이 끝났다는 뜻이다. Subscriber 수신은 기다리지 않는다.
messaging-execution category의 classic fanout `publish`와 달리, ChannelName만으로 owner MeshNode를
결정하며 caller가 MeshName을 추가로 넘기지 않는다.

**선택 기준.** Spot 상태 변화를 관찰자에게 알릴 때 쓴다. 구독자에게 직접 reply가 필요하면 이
항목이 아니라 `requestToSpot`을 쓴다.

---

## `addTimer` (Spot 코드 안)

Spot에 속한 주기 timer를 등록한다. `ZLinkSpotCommonContext.addTimer(...)`로 호출한다.

```ts
const timer = await context.addTimer(
  "room-tick",
  1_000,
  RoomTickHandler,
  { overrunPolicy: ZLinkTimerOverrunPolicy.SkipLateTicks },
);
```

**옵션.** `ZLinkTimerOptions`의 field는 다음과 같다.

| Field | 기본값 | 의미 |
| --- | --- | --- |
| `overrunPolicy` | `SkipLateTicks` | tick이 밀렸을 때 건너뛸지, 상한 안에서 따라잡을지, 다음 tick을 늦출지 |
| `maxCatchUpTicks` | 1 | `CatchUpBounded`일 때 한 번에 따라잡을 최대 tick 수 |
| `stopOnUnhandledException` | `false` | handler 예외 시 timer를 멈출지 여부 |

**완료 결과.** `ZLinkTimer`를 반환한다. Timer는 이 Spot에 속한 logical registration이라 relocation
때 자동으로 이전되며 application이 target에서 다시 등록할 필요가 없다. `cancel(signal?)` 또는
`dispose()`로 취소한다.

**선택 기준.** Spot 안에서 주기 작업이 필요할 때 쓴다.

---

## `runCpuWorker` / `runIoWorker` (Spot 코드 안)

Spot의 owner turn을 막지 않고 별도 worker에서 작업을 실행한다.

```ts
const result = await context
  .runCpuWorker((signal) => computeExpensiveScore(signal))
  .timeoutMs(2_000)
  .submit();
```

**옵션.** `ZLinkWorkerCall<T>`가 제공하는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.timeoutMs(durationMs)` | Worker option의 기본값 | 작업 완료 상한 |
| `.submit(signal?)` | terminal(택 1) | 완료까지 기다린다 |
| `.yield(signal?)` | terminal(택 1) | `SpotWide` handler 안에서만 유효 |

**완료 결과.** `T`를 반환하거나 timeout이면 `DeadlineExceeded`로 완료한다. Worker pool 크기
(`minThreads`/`maxThreads`)와 idle timeout은 host 시작 전에만 설정한다.

**선택 기준.** CPU-bound 계산은 `runCpuWorker`, I/O 대기가 있는 작업(`Promise<T>` 반환)은
`runIoWorker`를 쓴다. 둘 다 owner turn의 순차 실행을 막지 않으려는 목적이다.

---

## Handler 등록 (Spot 코드 안, decorator)

Spot이 받을 packet·request·구독·member Actor 메시지를 처리할 handler class를 decorator로
표시한다. NestJS provider discovery(`zlinkDiscoverProviders(...)`)가 module 안의 handler class를
찾아 등록한다.

```ts
@zlinkSpotRequestHandler({ spot: () => RoomSpot, packetName: "start-game" })
export class StartGameHandler implements ZLinkSpotRequestHandler<RoomSpot, StartGameReq, StartGameRes> {
  handle(spot: RoomSpot, request: StartGameReq): Promise<StartGameRes> { ... }
}
```

**옵션.** Handler가 처리하는 대상에 따라 구현하는 interface와 decorator가 갈린다.

| 대상 | Handler interface | Decorator |
| --- | --- | --- |
| User Spot 앞 one-way packet | `ZLinkSpotPacketHandler<TSpot, TMessage>` | `@zlinkSpotPacketHandler({ spot, packetName? })` |
| User Spot 앞 request | `ZLinkSpotRequestHandler<TSpot, TRequest, TReply>` | `@zlinkSpotRequestHandler({ spot, packetName })`(raw builder에서는 `@ZLinkSpotRequest(packetName?)` method decorator) |
| Logical Multicast 구독 이벤트 | `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` | `@zlinkSpotSubscriptionHandler({ spot, channelName, topic })`(raw는 `@ZLinkSpotSubscription(channelName, topic)`) |
| Spot 주기 timer | `ZLinkSpotTimerHandler<TSpot>` | `@zlinkSpotTimerHandler({ spot?, name?, periodMs?, options? })` |
| User Spot의 member Actor 앞 one-way packet | `ZLinkSpotActorSendHandler<TSpot, TActor, TMessage>` | `@zlinkSpotActorSendHandler({ spot, actor, packetName })`(raw는 `@ZLinkSpotActorSend(packetName?)`) |
| User Spot의 member Actor 앞 request | `ZLinkSpotActorRequestHandler<TSpot, TActor, TRequest, TReply>` | `@zlinkSpotActorRequestHandler({ spot, actor, packetName })` |
| Entry Spot의 member Actor 앞 one-way packet·request | `ZLinkEntrySpotActorSendHandler`/`ZLinkEntrySpotActorRequestHandler` | `@zlinkEntrySpotActorSendHandler`/`@zlinkEntrySpotActorRequestHandler({ entrySpot, actor, packetName })` |
| Entry Spot 자신의 packet·구독 | `ZLinkSpotPacketHandler`/`ZLinkSpotSubscriptionHandler`와 같은 모양 | `@zlinkEntrySpotPacketHandler`/`@zlinkEntrySpotSubscriptionHandler({ entrySpot, ... })` |
| Instance Spot 앞 packet(raw builder) | `ZLinkInstanceSpotHandlerRegistry.addPacket(handlerType)` | 없음(직접 등록) |

**완료 결과.** 반환값 없이 동기로 등록된다. `spot`/`entrySpot` field는 `Type<T> | (() => Type<T>)`
— 순환 참조를 피하려면 lazy resolver(`() => RoomSpot`)를 쓴다. 같은 owner의 handler key 중복은
startup 검증에서 configuration error로 드러난다.

**선택 기준.** 모든 Spot handler class에 정확히 하나의 decorator를 붙인다. Node·Channel handler는
topology-discovery category의 등록 항목을, STREAM session handler는 stream-session category를
참고한다.

---

## `outbound` — `sendToChannel` / `requestToChannel` (Spot 코드 안)

Spot 코드 안에서 ChannelName으로 one-way message를 보내거나 typed request/reply를 주고받는다.
`ZLinkSpotCommonContext.outbound`가 제공하며 messaging-execution category의 `sendToChannel`/
`requestToChannel`과 같은 모양이다.

```ts
const reply = await context.outbound
  .requestToChannel("leaderboard.api", new GetLeaderboard())
  .submit<Leaderboard>();
```

**옵션.** messaging-execution category의 `sendToChannel`/`requestToChannel`과 동일한 modifier를
받는다.

**완료 결과.** messaging-execution category의 완료 kind와 같다.

**선택 기준.** Spot이 외부 client가 아니라 자기 코드 안에서 다른 ChannelName의 handler를 호출해야
할 때 쓴다. 다른 Spot을 직접 호출하려면 `sendToSpot`/`requestToSpot`을 쓴다.

---

## `leaveActor` / `close` / `destroyActor` (Spot 코드 안, 종료·이탈)

Member Actor를 이 Spot에서 내보내거나, Spot 자신을 닫거나, Entry Spot에서 Actor를 파기한다.

```ts
await context.leaveActor(actor);        // User Spot: member Actor만 내보낸다
const closed = await context.close();   // User·Instance Spot: 이 Spot 자신을 닫는다
await entryContext.destroyActor(actor); // Entry Spot: Actor를 완전히 파기한다
```

**옵션.** 세 호출 모두 modifier가 없다 — 대상(`leaveActor`/`destroyActor`)과 선택적 `signal`만
받는다.

**완료 결과.** `leaveActor`(`ZLinkSpotContext` 전용)는 member Actor membership만 해제하고 Actor
자체는 파기하지 않는다. `close`(`ZLinkSpotContext`/`ZLinkInstanceSpotContext`)는 manager의
`close(spotRef)`(spot-instance category 앞부분 항목)와 같은 완료 kind를 쓰되, 이 Spot 자신을
대상으로 한다. `destroyActor`(`ZLinkEntrySpotContext` 전용)는 Actor를 완전히 파기한다 —
`leaveActor`와 달리 membership 해제가 아니라 Actor 자체를 없앤다.

**선택 기준.** Member Actor를 다른 곳으로 옮기지 않고 이 Spot에서만 빼려면 `leaveActor`를, Spot
자신을 스스로 종료하려면 `close`를, Entry Spot에서 더 이상 필요 없는 Actor를 완전히 없애려면
`destroyActor`를 쓴다.

---

## `relocationReady().defer()` (Spot 코드 안)

`ApplicationSignaled` readiness mode를 선택한 `SpotWide` Spot에서, relocation 경계를 다음
application turn 앞으로 미룬다.

```ts
context.relocationReady().defer();
```

**옵션.** 이 호출에는 modifier가 없다.

**완료 결과.** 반환값 없음. 현재 handler가 끝난 뒤 relocation 경계를 등록한다. 이동하지 않았거나
commit 전에 abort했으면 source에서 `Continued`, 이동했으면 target에서 `Relocated` completion을
optional `onRelocationReadyCompleted(...)`로 받는다(callback이 없으면 no-op으로 완료). `AnyTurnBoundary`
mode, `PerActor` Spot, Entry·Instance Spot, Spot turn 밖, 같은 turn의 중복 호출은
`InvalidOperation`으로 완료한다.

**선택 기준.** Application이 relocation 시점을 특정 turn 경계로 정밀하게 제어해야 할 때 쓴다.
기본 `AnyTurnBoundary` mode에서는 이 호출이 필요하지 않다.

---

전체 근거는
[Spot과 Instance Spot exact interface](../../common/spec/server/languages/node/interfaces/04-spots.ko.md)와
[STREAM, timer와 worker exact interface](../../common/spec/server/languages/node/interfaces/06-stream-worker.ko.md)를
참고한다.
