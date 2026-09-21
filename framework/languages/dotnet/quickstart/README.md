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

- .NET SDK 8.0 or newer. The project targets `net8.0`.
- Internet access to `nuget.org`. `nuget.config` uses that feed.
- No Redis or other external service.

## Download and install

Clone the [`zlink-dotnet-examples`](https://github.com/zlink-systems/zlink-dotnet-examples)
repository. Run the commands below from its `quickstart/` directory.

`Directory.Packages.props` pins `Zlink.Framework` and `Zlink.Framework.AspNetCore`.
The `Zlink` binding is not listed there; it resolves transitively from `Zlink.Framework`.

## Build

```bash title="linux"
dotnet build
```

```powershell title="windows"
dotnet build
```

## Run

Start the server first and the client second in separate terminals. The server listens on
`tcp://0.0.0.0:7101` and uses the `greeting` channel. The client listens on
`tcp://0.0.0.0:7102`, connects to `tcp://127.0.0.1:7101`, and serves
`GET /hello/{name}` on `http://127.0.0.1:5080`.

```bash title="linux"
dotnet run --project Server/Server.csproj > server.log 2>&1 &
dotnet run --project Client/Client.csproj > client.log 2>&1 &
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5080/hello/world > /dev/null && break; sleep 1; done
curl -sf http://127.0.0.1:5080/hello/world
```

```powershell title="windows"
Start-Process -NoNewWindow dotnet -ArgumentList 'run','--project','Server/Server.csproj' -RedirectStandardOutput server.log -RedirectStandardError server.err.log
Start-Process -NoNewWindow dotnet -ArgumentList 'run','--project','Client/Client.csproj' -RedirectStandardOutput client.log -RedirectStandardError client.err.log
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5080/hello/world; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
if ($LASTEXITCODE -ne 0) { throw 'quickstart did not come up' }
$answer
```

## Verify

```bash title="linux"
set -e
curl -sf http://127.0.0.1:5080/hello/world | grep -q '"hello, world"'
echo "quickstart=ok"
```

```powershell title="windows"
$answer = curl.exe -sf http://127.0.0.1:5080/hello/world
if ($LASTEXITCODE -ne 0 -or $answer -notmatch '"hello, world"') { throw 'quickstart failed' }
Write-Output 'quickstart=ok'
```

The endpoint returns `"hello, world"` with HTTP status 200.

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
