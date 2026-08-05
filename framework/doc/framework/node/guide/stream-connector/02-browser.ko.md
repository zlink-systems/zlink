# 02 — 브라우저

[← 목차](INDEX.ko.md) | [이전: 개요](01-overview.ko.md)

---

## 연결과 codec

package root에서 connector를 가져오고 필요한 payload codec을 생성 option에 넘긴다. inbound observer는
연결을 시작하기 전에 등록해야 한다.

```ts
import {
  zlinkStreamConnectorFactory,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import { zlinkStreamMessagePackCodec } from '@zlink-systems/framework-codec-msgpack';

const client = zlinkStreamConnectorFactory.create({
  endpoint: 'wss://game.example.com/stream', // 운영 연결은 브라우저가 인증서를 검증하는 wss를 사용한다.
  codec: zlinkStreamMessagePackCodec,        // 업무 payload의 encode와 decode를 이 codec이 담당한다.
  dispatchMode: ZlinkStreamDispatchMode.Immediate
});

client.observeInbound((message) => {
  console.log(message.name); // observer는 connect 전에 등록해야 첫 frame부터 관찰한다.
});

await client.connect(); // 플랫폼 WebSocket 연결이 준비될 때까지 기다린다.
```

개발 환경에서 암호화가 필요하지 않으면 `ws://`를 사용할 수 있다. `wss://`의 인증서 검증은
브라우저가 소유하며 connector에서 검증을 건너뛸 수 없다.

## dispatch

`Immediate`는 수신 callback을 connector가 바로 처리한다. 게임 main loop에서 처리 시점을 정해야 하면
`Manual`을 선택하고 해당 loop에서 `dispatch()`를 호출한다.

```ts
async function updateFrame(): Promise<void> {
  await client.dispatch(); // Manual mode에서는 main loop가 수신 handler 실행 시점을 결정한다.
}
```

## 관련 outbound의 flow 전달

브라우저에는 비동기 작업별 현재 값을 격리하는 표준 기능이 없다. inbound handler에서 시작한 관련
outbound만 `flowFrom(message)`로 표시한다. 표시하지 않은 timer나 UI callback의 outbound는 새
application flow를 시작하므로 동시에 실행되어도 inbound flow가 누출되지 않는다.

```ts
client.on('MatchAssigned', async (message) => {
  await refreshView(message.payload);

  client.send({ accepted: true })
    .packetName('MatchAccepted')
    .flowFrom(message) // await 뒤에도 이 outbound가 inbound flow에 속한다는 뜻을 명시한다.
    .submit();
});
```

application이 flow id를 전역 변수에 저장하거나 Promise와 timer 동작을 수정해서는 안 된다. 관련 없는
outbound에는 `flowFrom(...)`을 호출하지 않는다.
