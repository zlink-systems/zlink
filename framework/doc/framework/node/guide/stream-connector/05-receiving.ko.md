---
title: "packet 수신 · Node/TypeScript"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/stream-connector/05-receiving.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# packet 수신

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: packet 송신](04-sending.ko.md) | [다음: 연결 생명주기](06-lifecycle.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/stream-connector/05-receiving.ko.md) · [C#/.NET](../../../dotnet/guide/stream-connector/05-receiving.ko.md) · [Java](../../../java/guide/stream-connector/05-receiving.ko.md) · [Kotlin](../../../kotlin/guide/stream-connector/05-receiving.ko.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    서버가 먼저 보내는 packet을 handler로 받고, 등록을 원하는 시점에 해제할 수 있다.
    특정 packet 하나를 기다리는 표면과 수신 개수를 읽는 표면도 함께 사용한다.

서버가 보낸 packet은 handler로 넘어가거나 대기 표면이 가져가기 전까지 **수신 큐**에 머문다.
이 장은 그 큐에서 packet을 꺼내는 방법을 다룬다. 응답과 heartbeat는 이 큐를 거치지 않는다 —
응답은 기다리는 request로 바로 이어지고, heartbeat는 연결 유지에 사용한다.

## 1. handler 등록

handler가 받는 것은 payload만이 아니라 **message**다. message에는 packet 이름, decode한 payload,
metadata, 흐름 식별자가 함께 담긴다. 어떤 packet을 받을지는 payload 타입에서 정하거나 이름으로
지정한다.

```typescript
// TypeScript의 type은 실행 시점에 남지 않으므로 이름과 생성자를 함께 준다.
const subscription = connector.on<LeaderboardUpdate>(
  'leaderboard.update',
  message => { updateBoard(message.name, message.payload.rank); },
  LeaderboardUpdate
);
```

handler 안에서 같은 connector로 다시 송신할 수 있다. 그 송신은 처리 중인 message의 흐름을
이어받으므로 client의 log와 서버의 추적이 같은 흐름으로 이어진다.

## 2. 등록 해제

등록하면 **해제할 수 있는 값**이 돌아온다. 화면 하나의 수명에 맞춰 구독을 등록했다가 화면을
닫을 때 해제하는 client가 연결까지 다시 만들지 않게 하기 위해서다. 해제한 handler는 그 뒤로
실행되지 않고, 같은 값을 두 번 해제해도 오류가 아니다.

```typescript
subscription.dispose();
```

**돌려받은 값의 수명이 등록의 수명인지는 언어가 정한다.** 소유권을 값으로 나타내는 언어에서는
그 값이 사라지면 등록도 함께 사라지므로, 등록이 살아 있어야 하는 동안 그 값을 보관한다. 나머지
언어에서는 값을 버려도 명시적으로 해제할 때까지 등록이 유지된다.

연결 상태·오류·끊김 handler도 같은 값을 돌려준다. 이 handler들은
[연결 생명주기](06-lifecycle.ko.md)가 다룬다.

## 3. handler가 실행되는 시점

기본 설정에서 receive 경로는 handler를 직접 호출하지 않고 내부 큐에 넣는다. application이 pump를
호출하면 그때까지 쌓인 handler가 **pump를 호출한 실행 문맥에서** 실행된다. 게임 loop라면 frame마다
한 번 호출한다. pump는 쌓인 것을 처리하고 돌아오며, 새 packet이 도착하기를 기다리지 않는다.

즉시 실행으로 바꾸면 receive 경로에서 handler가 바로 실행되고 pump를 호출할 필요가 없다. 대신
느린 handler가 receive 경로를 막으므로 뒤따르는 수신 처리가 그만큼 늦어진다.

```typescript
while (running) {
  await connector.dispatch();
  renderFrame();
}
```

아래 대기 표면은 등록된 handler가 아니라 수신 큐를 직접 관측한다. 그래서 pump를 호출하지 않는
구성에서도 그대로 동작한다.

## 4. packet 하나 기다리기

시나리오의 특정 지점에서 packet 하나를 기다릴 때는 handler를 등록하지 않고 대기 표면을 사용한다.
조건에 맞는 packet을 소비하고 그 message를 돌려주며, 조건에 맞지 않는 packet은 큐에 남아 이후의
handler나 대기가 처리한다. timeout을 지정하지 않으면 connector의 기본 대기 timeout을 사용한다.

```typescript
const found = await connector
  .waitFor<MatchFound>('match.found')
  .where(message => message.payload.matchId === 'match-7f3a')
  .timeout(30_000)
  .submit();
```

**술어와 반환은 payload가 아니라 message를 다룬다.** payload만 받으면 술어가 packet 이름과
metadata를 보지 못하기 때문이다. 걸러야 하는 값이 payload 안에 있으면 술어 안에서 그 필드를
읽는다 — 상태 값 전용 표면은 두지 않는다.

## 5. 오지 않아야 하는 packet과 도착 순서

시나리오를 검증할 때는 "왔다"뿐 아니라 "오지 않았다"와 "순서대로 왔다"도 확인한다. 앞은 관찰
구간을 지정하고 그 구간 동안 그 packet이 오지 않는지 보고, 뒤는 술어를 순서대로 적용해 같은
이름의 packet이 그 순서로 도착했는지 확인한 뒤 message 목록을 돌려준다.

```typescript
await connector.expectNone<OrderChanged>('order.changed').within(100).run();

const steps = await connector
  .waitForSequence<OrderChanged>('order.changed')
  .expect(message => message.payload.status === 'paid')
  .expect(message => message.payload.status === 'shipped')
  .timeout(2_000)
  .run();
```

관찰 조건이 어긋난 실패 — 시간 안에 오지 않음, 오지 않아야 할 것이 도착함, 순서가 어긋남 — 는
검증 실패로 전달한다. 연결이 끝나 대기를 이어갈 수 없으면 연결 없음으로 전달한다. 관측이 어긋난
것과 관측할 자리가 사라진 것을 호출자가 구분해야 하기 때문이다.

## 6. 수신 개수

packet 이름별로 **받은 개수**를 읽는다. 소비해도 값이 줄지 않으므로, handler를 걸어 두고 pump한
뒤에도 그 이름으로 몇 건이 왔는지 판정할 수 있다. handler 실행 시점 설정과도 무관하게 packet이
도착한 시점에 값이 올라간다.

```typescript
const count = connector.receivedCount('leaderboard.update');
```

기준점은 연결이 성립한 시점이다. 연결이 성립하면 0에서 시작하고, 재연결하면 새 연결이므로 다시
0에서 시작한다. 이때 이전 연결에서 남아 있던 미소비 message도 함께 비운다 — 그러지 않으면 대기
표면이 끊기기 전의 packet을 새 연결의 것으로 돌려준다.

## 7. 수신 큐 — 한도를 두지 않는다

connector는 받은 것을 계속 받아서 처리한다. 큐에 한도를 두지 않고, message를 버리지 않으며,
큐가 길어졌다는 이유로 연결을 닫지도 않는다. connector는 socket을 직접 구현하지 않고 실행 환경이
주는 것을 사용하므로 읽기를 보류할 자리가 없다.

정상 동작하는 client에서는 message가 쌓이지 않는다. handler가 처리하거나 대기 표면이 소비하기
때문이다. 계속 쌓인다면 pump를 호출하지 않는 client 쪽 문제이므로, 수신 개수를 흐름 제어의 근거로
사용하지 않는다.

## 8. 다음 장

- 연결 상태와 재연결, 종료 사유 — [연결 생명주기](06-lifecycle.ko.md)
- 수신 경로에서 나는 오류 — [오류 처리](07-error-handling.ko.md)
