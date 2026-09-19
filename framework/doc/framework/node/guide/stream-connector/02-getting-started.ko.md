---
title: "설치와 첫 연결 · Node/TypeScript"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/stream-connector/02-getting-started.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 설치와 첫 연결

<!-- framework-adapter-nav:start -->
[목차](README.ko.md) | [이전: Stream Connector 개요](01-overview.ko.md) | [다음: Connector 옵션](03-connector-options.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/stream-connector/02-getting-started.ko.md) · [C#/.NET](../../../dotnet/guide/stream-connector/02-getting-started.ko.md) · [Java](../../../java/guide/stream-connector/02-getting-started.ko.md) · [Kotlin](../../../kotlin/guide/stream-connector/02-getting-started.ko.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    connector package를 프로젝트에 추가하고, 서버에 연결해 첫 packet을 주고받을 수 있다.
    이 장의 연결 코드는 `framework/languages/node/tutorial/StreamClient`에서 그대로 실행된다.

connector는 서버 framework와 별도로 배포되므로, client project는 connector package 하나만
참조한다. 이 장은 설치부터 첫 응답까지를 한 번에 따라 한다. 옵션 전체와 기본값은
[Connector 옵션](03-connector-options.ko.md)이 다룬다.

## 1. 설치

```bash
npm install @zlink-systems/stream-connector
```

## 2. 연결과 첫 request

connector는 endpoint와 timeout을 담은 option으로 만든다. 연결이 끝나야 packet을 보낼 수 있으므로
연결을 먼저 기다린다. request는 서버 응답이 도착할 때까지 기다린 뒤 응답 payload를 돌려준다.

```typescript
--8<-- "framework/languages/node/tutorial/StreamClient/main.ts:stream-client"
```

!!! warning "브라우저 계열 client는 `ws://`로 붙는다"

    네이티브 빌드는 `tcp://`·`tls://`·`ws://`·`wss://`를 모두 사용한다. 브라우저 계열은
    OS 소켓을 열 수 없으므로 `ws://`나 `wss://`만 사용하며, 서버 쪽 endpoint도 같은 scheme이어야
    한다.

## 3. 실행 결과

아래 명령은 tutorial(`framework/languages/node/tutorial`)의 Server를 그 README의 「실행」 절대로 띄운 상태에서 StreamClient를 실행한다. StreamClient는 Server의 stream endpoint에 연결한다.

```bash
dotnet run --project StreamClient/StreamClient.csproj
# connected: True
# round trip: 56ms
```

`connected`가 참이면 연결이 맺어진 것이다. 왕복 시간은 client가 보낸 시각을 서버가 그대로
돌려주어 잰 값이다.

## 4. 응답이 필요 없는 송신

응답을 기다리지 않는 packet은 send로 보낸다. 종결자를 호출해야 실제로 전송이 시작되고, 종결자는
전송이 실패하면 그 사실만 전달한다. 서버가 무엇을 했는지 알아야 하면 request를 사용한다.

```typescript
await connector
  .send(new ChatMessage('room-42', 'hello'))
  .packetName('chat.send')   // 생략하면 payload 생성자에서 이름을 정한다
  .submit();
```

## 5. 서버가 먼저 보내는 packet 받기

서버가 먼저 보내는 packet은 handler를 등록해 받는다. 등록하면 해제할 수 있는 값이 돌아오고, 그
값이 살아 있는 동안 handler가 유지된다. 기본 설정에서는 handler가 수신 시점에 바로 실행되지 않고,
application이 pump를 호출한 실행 문맥에서 실행된다. 게임 loop라면 frame마다 한 번 호출한다.

```typescript
const subscription = connector.on<LeaderboardUpdate>(
  'leaderboard.update',
  message => { console.log(message.payload.rank); },
  LeaderboardUpdate
);

await connector.dispatch();   // 쌓여 있던 handler를 실행하고 돌아온다
```

## 6. 다음 장

- 옵션 전체와 기본값 — [Connector 옵션](03-connector-options.ko.md)
- packet 이름·metadata·압축 — [packet 송신](04-sending.ko.md)
- 수신 큐와 대기 표면 — [packet 수신](05-receiving.ko.md)
