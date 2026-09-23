# Browser

[← Table Of Contents](INDEX.en.md) | [Previous: Overview](01-overview.en.md) | [Next: Unity WebGL →](09-unity-webgl.en.md)

---

## Connecting And Codec

Import the connector from the package root and pass the payload codec you need as a creation
option.

```ts
import {
  zlinkStreamConnectorFactory,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import { zlinkStreamMessagePackCodec } from '@zlink-systems/framework-codec-msgpack';

const client = zlinkStreamConnectorFactory.create({
  endpoint: 'wss://game.example.com/stream', // Production connections use wss, whose certificate the browser verifies.
  codec: zlinkStreamMessagePackCodec,        // This codec handles encoding and decoding of the business payload.
  dispatchMode: ZlinkStreamDispatchMode.Immediate
});

await client.connect(); // Waits until the platform WebSocket connection is ready.
```

If encryption isn't needed in a development environment, `ws://` can be used. `wss://`'s
certificate verification is owned by the browser, and the connector can't skip it.

## Dispatch

`Immediate` has the connector process the receive callback right away. If you need to control the
processing moment from the game's main loop, choose `Manual` and call `dispatch()` from that loop.

```ts
async function updateFrame(): Promise<void> {
  await client.dispatch(); // In Manual mode, the main loop decides when the receive handler runs.
}
```
