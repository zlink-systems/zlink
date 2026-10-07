# StreamClient Protobuf Tutorial

One codec receives both generated `Ping` and `Pong` types through `on(Type, handler)`.
Replies use `submit(Pong)`.
The verification program runs the browser entry point with Node's WebSocket.
Version 0.28.0 ignores handler types; before the fix is released, use the local connector and codec packages.

## Installation and Build

Use Node.js 22 or later and npm. Run from this directory.

```bash
npm ci
npm run prepare:local
npm run build
```

The build generates a protobufjs static-module and TypeScript declarations under `generated/`, then
copies them to `dist/StreamClient/generated/` alongside the compiled code. Generated files are not committed.

## Execution

To verify without a server, run the verification WebSocket peer and tutorial together.

```bash
npm run prepare:local
npm run protobuf:check
# protobuf: Ping=hello, Pong.rank=3, reply.rank=7
```

The verification peer uses an ephemeral local port. It sends `Ping` and `Pong { rank: 3 }` pushes and
replies to a `Ping` request with `Pong { rank: 7 }`. The checks also cover incorrect codec numbers and malformed bytes.

If a server implementing the same protocol is available, set its address in the environment.

```powershell
$env:STREAM_PROTOBUF_ENDPOINT = 'ws://127.0.0.1:7721'
npm run protobuf
```

The existing JSON tutorial Server does not implement this Protobuf protocol.
The existing `npm start` runs the JSON flow using the procedure in the parent tutorial README.

## Guides

- [Node Protobuf Messaging](https://zlink.systems/node/guide/stream-connector/40-protobuf/)
- [Node Protobuf Codecs and Types](https://zlink.systems/node/guide/stream-connector/41-protobuf-codecs/)
