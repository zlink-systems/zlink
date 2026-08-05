# C++ Stream Connector

> **⚠️ This guide is not current.** The only guide that has finished review and upkeep right now is
> the [`.NET` guide](../../../dotnet/README.en.md). This document reflects an earlier state, and
> **once the `.NET` guide is finished, this document will be deleted and rewritten based on it.**
>
> **When confirming the contract, don't trust this document — check the [spec tree](../../../common/spec/README.en.md).**

This is the documentation entry point for the C++ STREAM client connector family. It targets
**natively-built game engines** (Unreal, Godot GDExtension, Axmol), general C++ applications, and
server e2e/perf clients.

| Document | Content |
|------|------|
| [Guide INDEX](INDEX.en.md) | Overview, getting started, options, send/receive, lifecycle, engine adapters, packaging, performance |
| [core — async runtime](core/guide/async-runtime.ko.md) | The no-exception, no-coroutine core runtime |
| [e2e-client — coroutine client](e2e-client/guide/coroutine-client.ko.md) | A coroutine helper for server e2e/perf use |
| [Stream Connector Common Spec](../../../common/spec/stream-connector/32-stream-connector.en.md) | **Authoritative** — target environment, transport, wire contract |

> **This connector can't be used for a web (WASM) build.** Cocos Creator web and Godot Web use the
> [Node.js/TypeScript connector](../../../node/guide/stream-connector/README.en.md).
