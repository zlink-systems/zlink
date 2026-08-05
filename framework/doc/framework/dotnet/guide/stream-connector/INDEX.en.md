# .NET Stream Connector Guide

This is the official user guide for the `.NET` STREAM client connector
(`Systems.Zlink.Stream.Connector`). It targets desktop/server applications and **natively-built
game engines** (Unity, Godot C#).

## Table Of Contents

| Document | Content |
|------|------|
| [01 — Overview](01-overview.en.md) | Target execution environment, deployment unit, per-engine connector responsibility |
| [02 — Unity (Native Build)](02-unity.en.md) | Pumping `Dispatch.Async()` from a `MonoBehaviour`, pausing, coroutine projects |
| [03 — Godot C#](03-godot-csharp.en.md) | Pumping from `Node._Process`, signal integration, shutdown handling |

The connector's API surface (options, send/request, codec, errors) is covered by the
[.NET Public Contract](../../../common/spec/stream-connector/languages/dotnet/03-stream-connector.en.md)
and [.NET Framework Guide 09 — STREAM](../../../common/guide/server/09-stream.en.md). This guide
focuses on **engine integration**.

## Connectors For Other Languages

| Document | Target |
|------|------|
| [C++ Stream Connector Guide](../../../cpp/guide/stream-connector/INDEX.en.md) | Unreal, Godot (GDExtension), Axmol, general C++ |
| [Node/TypeScript Stream Connector Guide](../../../node/guide/stream-connector/INDEX.en.md) | **Browser, Unity WebGL, Cocos web**, Node |

**If you build for web (browser/WASM), you use the TypeScript connector regardless of language.**
The judgment criterion is owned by
[Stream Connector Common Spec §2](../../../common/spec/stream-connector/32-stream-connector.en.md).
