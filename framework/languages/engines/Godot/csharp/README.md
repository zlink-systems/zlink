**English** | [한국어](README.ko.md)

# ZLink Engine Lobby Godot sample

This Godot 4 C# scene connects to the shared Engine Lobby server. `EngineLobbyNode` dispatches
connector callbacks on the Godot main thread each frame. The client sends `PingReq`, `JoinReq`,
and `ChatMsg`, then displays `ChatNotify` in the scene.

Packet names and JSON fields follow the [shared Engine Lobby contract](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.md).

## Versions and packages

- Godot 4.4.1 .NET edition
- .NET 8 SDK and `Godot.NET.Sdk` 4.4.1
- `Zlink.Stream.Connector`: the `PackageReference` in [`EngineLobby.csproj`](EngineLobby.csproj) owns the version.

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

## Run and verify the scene

1. Install, build, and start the server using the README in the separate [zlink-engine-server repository](https://github.com/zlink-systems/zlink-engine-server/blob/main/README.md). Read `.run/stream.port` there.
2. Set the root node's `Endpoint` in `EngineLobby.tscn` to `ws://127.0.0.1:<stream.port>`. The default is `ws://127.0.0.1:22700`.
3. Build the project in the Godot 4.4.1 .NET editor, then run the scene. These Linux commands perform the same editor build and scene run without a display. Set `GODOT` to the Godot 4.4.1 .NET executable path.

```bash
"$GODOT" --headless --path . --build-solutions --quit-after 120
"$GODOT" --headless --path . --quit-after 600
```

On Windows, set `$godot` to the Godot 4.4.1 .NET `_console.exe` path and run these PowerShell commands. Output is written to `godot-csharp.log` and `godot-csharp.err.log`.

```powershell
$godot = 'C:\path\to\Godot_v4.4.1-stable_mono_win64_console.exe'
Start-Process $godot -ArgumentList @('--headless','--path',(Get-Location).Path,'--build-solutions','--quit-after','120') -Wait -WindowStyle Hidden -RedirectStandardOutput godot-build.log -RedirectStandardError godot-build.err.log
Start-Process $godot -ArgumentList @('--headless','--path',(Get-Location).Path,'--quit-after','600') -Wait -WindowStyle Hidden -RedirectStandardOutput godot-csharp.log -RedirectStandardError godot-csharp.err.log
```

Check Godot output for `JoinRes name=godot-player` (the client checks the Ping reply before it joins) and `ChatNotify name=godot-player text=hello from Godot`. The label also changes from `joined as godot-player (...)` to `godot-player: hello from Godot`. Check for a new `client connected:` line in the server log during the scene run. Stop the server using its README after verification.

## Headless connector validation

`Validation` compiles the same `EngineLobbyClient.cs` used by the scene. Given a running server's
WebSocket endpoint, it checks Ping, Join, and both clients' `ChatNotify` payloads.

```bash
dotnet run --project Validation/EngineLobbyValidation.csproj -c Release -- \
  ws://127.0.0.1:<stream.port>
```

Success prints `godot-engine-lobby-validation=ok`. This check does not run the Godot scene tree,
load the C# assembly in the editor, or verify the rendered label.
