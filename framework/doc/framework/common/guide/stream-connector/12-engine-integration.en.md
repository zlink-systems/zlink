# Game Engine Integration

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
| Godot C# | `.NET` | [Godot C#](../../../dotnet/guide/stream-connector/09-godot-csharp.en.md) | The Godot C# chapter does not cover deployment |
| Godot C++ | The C++ connector's GDExtension adapter | [Engine adapters](../../../cpp/guide/stream-connector/09-engine-adapters.en.md) | [C++ packaging](../../../cpp/guide/stream-connector/10-packaging.en.md) |
| Axmol | The C++ connector's Axmol adapter | [Engine adapters](../../../cpp/guide/stream-connector/09-engine-adapters.en.md) | [C++ packaging](../../../cpp/guide/stream-connector/10-packaging.en.md) |
| Cocos Creator web | TypeScript connector | [Browser](../../../node/guide/stream-connector/08-browser.en.md) | [Cocos Creator sample](https://github.com/zlink-systems/zlink/blob/main/framework/languages/engines/CocosCreator/README.md) |

## 1. Engine Lobby Sample

The [Engine Lobby contract](../../../common/sample/engine-lobby/README.md) defines the packets
exchanged by the server and clients. Its [implementation structure](../../../common/sample/engine-lobby/README.md#8-implementation-structure)
table decides which engine clients exist and which mirror repository ships each one. This chapter
quotes the Unity and Unreal clients.

The Unity sample uses the same client source for native and WebGL builds. It connects,
waits for a `JoinReq` response, and sends a `ChatMsg`.

```csharp title="Unity/Assets/EngineLobby/ZLinkClient.cs"
--8<-- "framework/languages/engines/Unity/Assets/EngineLobby/ZLinkClient.cs:connect"
```

```csharp title="Unity/Assets/EngineLobby/ZLinkClient.cs"
--8<-- "framework/languages/engines/Unity/Assets/EngineLobby/ZLinkClient.cs:send"
```

The `On<ChatNotify>` callback runs when `Update()` calls `Dispatch.Async()`.

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

The connector is pumped in `Tick()`. Its `ChatNotify` delegate updates the display.

```cpp title="Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp"
--8<-- "framework/languages/engines/Unreal/Source/EngineLobby/Private/EngineLobbyClientActor.cpp:receive"
```
