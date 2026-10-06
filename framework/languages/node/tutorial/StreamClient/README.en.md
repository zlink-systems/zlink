# StreamClient Protobuf Tutorial

Send and receive pushes using `Ping` generated from `.proto`, and decode request replies as `Pong`.
The fixed-type codec handles `Ping`; replies use `submitEncoded()` and `fromProto(..., Pong)`.
This example uses Node as a verification environment for the browser connector.

## Installation and Build

Use Node.js 22 or later and npm. Run from this directory.

```bash
npm ci
npm run build
```

The build generates a protobufjs static-module and TypeScript declarations under `generated/`, then
copies them to `dist/StreamClient/generated/` alongside the compiled code. Generated files are not committed.

## Execution

To verify without a server, run the verification WebSocket peer and tutorial together.

```bash
npm run protobuf:check
# protobuf: push=hello, reply.rank=7
```

The verification peer uses an ephemeral local port. It echoes a `Ping` send as a `Ping` push and replies
to a `Ping` request with `Pong { rank: 7 }`. The checks also cover incorrect codec numbers and malformed bytes.

If a server implementing the same protocol is available, set its address in the environment.

```powershell
$env:STREAM_PROTOBUF_ENDPOINT = 'ws://127.0.0.1:7721'
npm run protobuf
```

The existing JSON tutorial Server does not implement this Protobuf protocol.
The existing `npm start` runs the JSON flow using the procedure in the parent tutorial README.

## Guides

- [Node Protobuf Messaging](../../../../doc/framework/node/guide/stream-connector/40-protobuf.en.md)
- [Node Protobuf Codecs and Types](../../../../doc/framework/node/guide/stream-connector/41-protobuf-codecs.en.md)
