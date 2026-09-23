**English** | [한국어](README.ko.md)

# ZLink Engine Lobby Godot sample

This Godot 4 C# scene connects to the shared Engine Lobby server. `EngineLobbyNode` dispatches
connector callbacks on the Godot main thread each frame. The client sends `PingReq`, `JoinReq`,
and `ChatMsg`, then displays `ChatNotify` in the scene.

Packet names and JSON fields follow the [shared Engine Lobby contract](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.md).

## Versions and packages

- Godot 4.4.1 .NET edition
- .NET 8 SDK and `Godot.NET.Sdk` 4.4.1
- `Zlink.Stream.Connector` 0.22.0

`EngineLobby.csproj` restores both NuGet packages. Godot Web export is outside the scope of this
.NET project.

## Install

```bash
dotnet restore EngineLobby.csproj
```

## Build

```bash
dotnet build EngineLobby.csproj -c Release
dotnet build Validation/EngineLobbyValidation.csproj -c Release
```

## Run the scene

1. Start the shared `zlink-engine-server` sample and read its `stream.port`. In the monorepo,
   run `./run_sample.sh build` and `./run_sample.sh run` from `../Server`.
2. Open `project.godot` in the Godot 4.4.1 .NET editor and build the C# project.
3. Set the root node's `Endpoint` in `EngineLobby.tscn` to
   `ws://127.0.0.1:<stream.port>`, then run the scene.
4. Confirm that the label changes from `joined as godot-player (...)` to
   `godot-player: hello from Godot`. `_Process` calls `Dispatch.Async()` for the notification.
5. If you used the monorepo server, run `./run_sample.sh stop` from `../Server`.

## Headless connector validation

`Validation` compiles the same `EngineLobbyClient.cs` used by the scene. Given a running server's
WebSocket endpoint, it checks Ping, Join, and both clients' `ChatNotify` payloads.

```bash
dotnet run --project Validation/EngineLobbyValidation.csproj -c Release -- \
  ws://127.0.0.1:<stream.port>
```

Success prints `godot-engine-lobby-validation=ok`. This check does not run the Godot scene tree,
load the C# assembly in the editor, or verify the rendered label.

## Validation on this machine

The Godot C# project and headless validation project built in WSL. Two clients connected to the
shared .NET server and verified Ping, Join, and `ChatNotify` payloads. The Godot editor is not
installed here, so the scene and label were not run.
