---
title: "Game Engine Integration · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/stream-connector/12-engine-integration.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Game Engine Integration

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Unity WebGL](09-unity-webgl.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/stream-connector/12-engine-integration.en.md) · [C#/.NET](../../../dotnet/guide/stream-connector/12-engine-integration.en.md) · [Java](../../../java/guide/stream-connector/12-engine-integration.en.md) · [Kotlin](../../../kotlin/guide/stream-connector/12-engine-integration.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can choose the connector for an engine and build target, then find the installation,
    packet exchange, packaging guides, and runnable Engine Lobby sample.

A game client connects to a STREAM server. Unity uses a different connector for each build
target. Unreal and Godot C# use the C++ and .NET connectors, respectively. Follow the chapter
for your engine for installation, connection, sending and receiving, and main-thread dispatch.
The packaging column points to the package and build steps for each target.

| Engine and target | Connector | Usage guide | Packaging |
|---|---|---|---|
| Unity native | `.NET` | [Unity native](../../../dotnet/guide/stream-connector/08-unity.en.md) | [Unity sample native build](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unity/README.md) |
| Unity WebGL | TypeScript connector and UPM adapter | [Unity WebGL](../../../node/guide/stream-connector/09-unity-webgl.en.md) | [Unity sample WebGL build](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unity/README.md) |
| Unreal | C++ connector's Unreal plugin | [Engine adapters](../../../cpp/guide/stream-connector/09-engine-adapters.en.md) | [C++ packaging](../../../cpp/guide/stream-connector/10-packaging.en.md) |
| Godot C# | `.NET` | [Godot C#](../../../dotnet/guide/stream-connector/09-godot-csharp.en.md) | This chapter has no deployment steps |

## 1. Engine Lobby Sample

The [Engine Lobby contract](../../../common/sample/engine-lobby/README.md) defines the packets
exchanged by the server and clients. The [.NET server](https://github.com/zlink-systems/zlink-engine-server),
[Unity project](https://github.com/zlink-systems/zlink-unity-examples), and
[Unreal project](https://github.com/zlink-systems/zlink-unreal-examples) also have separate
repositories. The source in this repository and run steps are in the
[server](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Server/README.md),
[Unity](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unity/README.md), and
[Unreal](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/Unreal/README.md) READMEs.

The Unity sample uses the same client source for native and WebGL builds. It connects,
waits for a `JoinReq` response, and sends a `ChatMsg`.

```csharp title="Unity/Assets/EngineLobby/ZLinkClient.cs"
--8<-- "framework/languages/engines/Unity/Assets/EngineLobby/ZLinkClient.cs:connect"
```

```csharp title="Unity/Assets/EngineLobby/ZLinkClient.cs"
--8<-- "framework/languages/engines/Unity/Assets/EngineLobby/ZLinkClient.cs:send"
```

The `On<ChatNotify>` callback runs when `update()` calls `Dispatch.Async()`.

```csharp title="Unity/Assets/EngineLobby/ZLinkClient.cs"
--8<-- "framework/languages/engines/Unity/Assets/EngineLobby/ZLinkClient.cs:receive"
```

The Unreal client creates the plugin connector, connects, and starts joining the lobby after a
`PingReq` response.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:connect-call"
```

`JoinReq` waits for a response. `ChatMsg` is a one-way send.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:send"
```

The connector is pumped in `tick()`. Its `ChatNotify` delegate updates the display.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:receive"
```
