# 브라우저

[← 목차](INDEX.ko.md) | [이전: 개요](01-overview.ko.md) | [다음: Unity WebGL →](09-unity-webgl.ko.md)

---

## 연결과 codec

package root에서 connector를 가져오고 필요한 payload codec을 생성 option에 넘긴다.

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
