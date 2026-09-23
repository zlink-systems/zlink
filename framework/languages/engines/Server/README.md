**English** | [한국어](README.ko.md)

# ZLink Engine Lobby server

This .NET server lets Unity and other game-engine clients join a shared lobby and exchange chat.
One STREAM listener provides the `PingReq`/`PingRes`, `JoinReq`/`JoinRes`, and
`ChatMsg`/`ChatNotify` flow; an
Actor bound to each session performs lobby fan-out.

Prerequisites are the .NET 8 SDK, Docker, curl, and Python 3. This directory references only public
NuGet packages, so the same commands work after the directory is exported from the monorepo.

`EngineLobby.sln` contains the executable `Server/EngineLobby.Server.csproj` and the message contracts
in `Shared/EngineLobby.Shared.csproj`.

## Download and install

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh install
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action install
```

## Build

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh build
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action build
```

## Run

The runner creates a Redis container dedicated to this run, allocates RouteMesh, WebSocket STREAM,
and HTTP readiness ports, and starts the server.

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh run
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action run
```

## Verify

The step checks readiness with curl first. A small C# probe in the Server project then uses two
connectors to verify `PingReq`, both `JoinReq` replies, `ChatMsg`, and both clients'
`ChatNotify` payloads.

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh verify
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action verify
```

The final line is `engine-lobby-probe=ok` on success.

## Stop

This stops only the exact server PID and Redis container ID recorded by the Run step.

**Linux · macOS · WSL — bash**

```bash title="linux"
./run_sample.sh stop
```

**Windows — PowerShell 7**

```powershell title="windows"
./run_sample.ps1 -Action stop
```

Failure diagnostics remain in `.run/server.log` and `.run/server.err.log`.
