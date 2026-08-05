# 02 — Browser

[← Table Of Contents](INDEX.en.md) | [Previous: Overview](01-overview.en.md)

---

## Connecting And Codec

Import the connector from the package root and pass the payload codec you need as a creation
option. An inbound observer must be registered before starting the connection.

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

client.observeInbound((message) => {
  console.log(message.name); // The observer must be registered before connect, to observe from the first frame.
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

## Flow Propagation For Related Outbound

The browser has no standard feature that isolates the current value per asynchronous task. Only mark
an outbound related to an inbound handler that started it with `flowFrom(message)`. An unmarked
outbound from a timer or UI callback starts a new application flow, so even if it runs concurrently,
the inbound flow doesn't leak.

```ts
client.on('MatchAssigned', async (message) => {
  await refreshView(message.payload);

  client.send({ accepted: true })
    .packetName('MatchAccepted')
    .flowFrom(message) // Even after the await, this makes explicit that this outbound belongs to the inbound flow.
    .submit();
});
```

The application must not store the flow id in a global variable, or modify how Promises and timers
behave. Don't call `flowFrom(...)` on an unrelated outbound.
