# TypeScript Stream Connector

> **⚠️ This guide is not current.** The only guide that has finished review and upkeep right now is
> the [`.NET` guide](../../../dotnet/README.en.md). This document reflects an earlier state, and
> **once the `.NET` guide is finished, this document will be deleted and rewritten based on it.**
>
> **When confirming the contract, don't trust this document — check the [spec tree](../../../common/spec/README.en.md).**

This is the documentation entry point for the TypeScript STREAM client connector
(`@zlink-systems/stream-connector`). The target is browser web clients and browser-executed builds
like Unity WebGL, Cocos Creator web, and Godot Web. Node.js is not a connector execution
environment — it's used only to run the test runner and server processes.

| Document | Content |
|------|------|
| [Guide INDEX](INDEX.en.md) | Browser connection, codec, dispatch, and flow delivery |
| [TypeScript Public Contract](../../../common/spec/stream-connector/languages/typescript/03-stream-connector.en.md) | Exact public types and package root |
| [Stream Connector Common Spec](../../../common/spec/stream-connector/32-stream-connector.en.md) | Target environment, transport, and wire contract |

The package root provides `ws` and `wss` connections through the platform `WebSocket`. It doesn't
provide a `/browser` subpath or a Node socket implementation.
