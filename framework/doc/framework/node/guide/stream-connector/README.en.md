# TypeScript Stream Connector

This is the documentation entry point for the TypeScript STREAM client connector
(`@zlink-systems/stream-connector`). The target is browser web clients and browser-executed builds
like Unity WebGL, Cocos Creator web, and Godot Web. Node.js is not the connector's product
runtime — it handles only server processes and the browser test runner.

| Document | Content |
|------|------|
| [Guide INDEX](INDEX.en.md) | Browser connection, codec, dispatch, and flow delivery |
| [03 — Unity WebGL](03-unity-webgl.en.md) | The `com.zlink.stream-connector.webgl` UPM adapter |
| [TypeScript Public Contract](../../../common/spec/stream-connector/languages/typescript/03-stream-connector.en.md) | Exact public types and package root |
| [Stream Connector Common Spec](../../../common/spec/stream-connector/32-stream-connector.en.md) | Target environment, transport, and wire contract |

The package root provides `ws` and `wss` connections through the platform `WebSocket`. It doesn't
provide a `/browser` subpath or a Node socket implementation.
