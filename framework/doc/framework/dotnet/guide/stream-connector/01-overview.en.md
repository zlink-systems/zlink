# 01 — Overview

[← Table Of Contents](INDEX.en.md) | [Next: Unity →](02-unity.en.md)

---

The `.NET` Stream Connector is a client-side library that connects to a ZLink STREAM server.
Desktop/server applications and **natively-built game engines** use the same STREAM protocol.

## Target Execution Environment

First, you need to decide **which environment you're building for.** The environment's constraints
decide which connector to use.

| Target | Connector | Reason |
|------|-----------|------|
| Desktop/server application | **`.NET` connector** | Uses the OS socket directly |
| **Unity — native build** (PC, mobile, console) | **`.NET` connector** | Same reason. Only the main-thread constraint is added ([02](02-unity.en.md)) |
| **Godot C# — native build** | **`.NET` connector** | Same reason ([03](03-godot-csharp.en.md)) |
| **Unity — WebGL build** | **TypeScript connector** | The browser sandbox can't open an OS socket |
| **Godot — Web build** | **TypeScript connector** | Same reason |

**The moment you build for web (browser/WASM), you use the TypeScript connector, regardless of
language.** That's because no language can open an OS socket in the browser sandbox. For a web
build, see the [Node/TypeScript Connector Guide](../../../node/guide/stream-connector/INDEX.en.md).

## Deployment Unit

| Artifact | Distribution Format | Main Users |
|--------|-----------|-------------|
| `Systems.Zlink.Stream.Connector` | NuGet | Desktop/server applications, Unity (native), Godot C# |

**Unity and Godot C# don't have a dedicated package.** They use the NuGet package above as-is. What
engine integration needs isn't a separate API — it's just **where to pump dispatch.**

## The Game Engine's Constraint: The Main Thread

Engine objects (`GameObject`, `Node`, the scene tree) must not be touched off the main thread. So
the connector's default dispatch mode is **`Manual`**. In this mode, the handler and event callbacks
run only when the user **explicitly pumps on the main thread.**

| Engine | Pump Location |
|------|-----------|
| Unity | `Dispatch.Async()` in `MonoBehaviour.Update()` |
| Godot C# | `Dispatch.Async()` in `Node._Process(double)` |
| General application | Switch to `Dispatch.Immediate` if pumping isn't needed |

**If you don't pump, the handler and events don't run.** Check `PendingDispatchCount` to see how
many callbacks haven't been processed yet.

## Transport Support

| scheme | transport |
|--------|-----------|
| `tcp://host:port` | TCP |
| `tls://host:port` | TLS over TCP |
| `ws://host:port/path` | WebSocket |
| `wss://host:port/path` | WebSocket over TLS |

A native build uses all four.

## Relationship With The Server Framework

The connector is a client library that connects to a STREAM server. It doesn't depend on the server
framework package.

The wire contract and connection lifecycle's authority is the
[Stream Connector Common Spec](../../../common/spec/stream-connector/32-stream-connector.en.md), and
the `.NET` public surface's authority is the
[.NET Public Contract](../../../common/spec/stream-connector/languages/dotnet/03-stream-connector.en.md).
