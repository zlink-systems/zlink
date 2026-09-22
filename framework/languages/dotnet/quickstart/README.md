[English](./README.md) | [한국어](./README.ko.md)

# ZLink .NET quickstart

This is the smallest project: two processes call each other once over a channel, with no
location store because the client names the server endpoint directly. The site page
`framework/doc/framework/dotnet/quickstart.ko.md` reads its code blocks from these files.
This directory is `quickstart/` in the `zlink-dotnet-examples` repository.

| | Purpose |
|---|---|
| **quickstart** (here) | Installs packages and reaches the first reply |
| tutorial (`tutorial/`) | Adds features one at a time. The feature guides read this code |
| samples (`samples/`) | Shows applications with a complete business flow |

## Prerequisites

Bash blocks run on Linux, macOS, and WSL; PowerShell blocks run on Windows PowerShell 7. `cmd` is not supported.

- .NET SDK 8.0 or newer. The project targets `net8.0`.
- Internet access to `nuget.org`. `nuget.config` uses that feed.
- No Redis or other external service.

## Download and install

Clone the [`zlink-dotnet-examples`](https://github.com/zlink-systems/zlink-dotnet-examples)
repository. Run the commands below from its `quickstart/` directory.

`Directory.Packages.props` pins `Zlink.Framework` and `Zlink.Framework.AspNetCore`.
The `Zlink` binding is not listed there; it resolves transitively from `Zlink.Framework`.

## Build

**Linux · macOS · WSL — bash**

```bash title="linux"
dotnet build
```

**Windows — PowerShell 7**

```powershell title="windows"
dotnet build
```

## Run

Start the server first and the client second in separate terminals. The server listens on
`tcp://0.0.0.0:7101` and uses the `greeting` channel. The client listens on
`tcp://0.0.0.0:7102`, connects to `tcp://127.0.0.1:7101`, and serves
`GET /hello/{name}` on `http://127.0.0.1:5080`.

**Linux · macOS · WSL — bash**

```bash title="linux"
dotnet run --project Server/Server.csproj > server.log 2>&1 &
echo $! > server.pid
dotnet run --project Client/Client.csproj > client.log 2>&1 &
echo $! > client.pid
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5080/hello/world > /dev/null && break; sleep 1; done
```

**Windows — PowerShell 7**

```powershell title="windows"
$server = Start-Process -NoNewWindow dotnet -ArgumentList 'run','--project','Server/Server.csproj' -RedirectStandardOutput server.log -RedirectStandardError server.err.log -PassThru
$server.Id | Set-Content server.pid
$client = Start-Process -NoNewWindow dotnet -ArgumentList 'run','--project','Client/Client.csproj' -RedirectStandardOutput client.log -RedirectStandardError client.err.log -PassThru
$client.Id | Set-Content client.pid
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5080/hello/world; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
if ($LASTEXITCODE -ne 0) { throw 'quickstart did not come up' }
```

## Verify

Examples smoke runs this block exactly as written.

**Linux · macOS · WSL — bash**

```bash title="linux"
set -e
curl -sf http://127.0.0.1:5080/hello/world | grep -q '"hello, world"'
echo "quickstart=ok"
```

**Windows — PowerShell 7**

```powershell title="windows"
$answer = curl.exe -sf http://127.0.0.1:5080/hello/world
if ($LASTEXITCODE -ne 0 -or $answer -notmatch '"hello, world"') { throw 'quickstart failed' }
Write-Output 'quickstart=ok'
```

The endpoint returns `"hello, world"` with HTTP status 200.

## Stop

Stop the processes started by the Run section.

**Linux · macOS · WSL — bash**

```bash title="linux"
for pid in "$(cat client.pid)" "$(cat server.pid)"; do
  pkill -TERM -P "$pid" 2>/dev/null || true
  kill "$pid" 2>/dev/null || true
done
```

**Windows — PowerShell 7**

```powershell title="windows"
Get-Content client.pid, server.pid | ForEach-Object {
  if ($_ -match '^\d+$') { taskkill /PID $_ /T /F 2>$null | Out-Null }
}
Get-Job | Stop-Job -ErrorAction SilentlyContinue
```

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Ports 7101, 7102, or 5080 are busy | Stop the earlier Server or Client process |
| The curl request cannot connect | Start Server, then Client, and inspect process output |
| The request has no target | Match the client's `Connect` and server's `Listen` endpoints |
| The handler is not called | Register `HelloHandler` explicitly on the `greeting` channel |

## Project layout

| Path | Contents |
|---|---|
| `Shared/` | The `Hello` and `Greeting` record contracts shared by both processes |
| `Server/` | Registers the `greeting` handler and listens on port 7101 |
| `Client/` | Connects to the server and exposes `GET /hello/{name}` on port 5080 |
| `Directory.Packages.props` | Central pins for the framework packages |
| `nuget.config` | The NuGet package source configuration |

## What to carry into your own project

- `Directory.Packages.props`, including the explicit `Zlink.Framework` and
  `Zlink.Framework.AspNetCore` versions. Leave the `Zlink` binding to the framework's
  transitive dependency unless the package contract requires a direct reference.
- `Shared/Contracts.cs` and its record-based message contracts.
- `Server/Program.cs` and the `AddZLinkFramework` block: the mesh name, `Listen`,
  `Channel("greeting").Server()`, and explicit `AddRequestHandler` registration.
- `Client/Program.cs` and the `AddZLinkFramework` block: `Channel("greeting").Client()`,
  `PeerConnections.Connect`, and `RequestToChannel(...).Async<Greeting>()`.
- A production service normally replaces the manual peer connection with a location store,
  such as Redis. This quickstart omits that service dependency.
