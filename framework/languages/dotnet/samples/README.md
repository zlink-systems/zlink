# ZLink Framework .NET Samples

.NET samples demonstrate the public 11.0.0 framework contract through separate
server-role processes and executable client scenarios. Their domain flows and
verification rules follow the
[common sample scenarios](../../../doc/framework/common/sample/README.ko.md).

## Samples

| Sample | Purpose | Peer topology |
|---|---|---|
| [TicTacToe](TicTacToe) | Two API roles and two Play roles demonstrate room lookup, Actor turns, and real-time game messages. | Manual MeshNode peers; Redis room route store |
| [Bingo](Bingo) | Session admission, Entry and room Spots, Actor binding, timer draws, and bound-session notifications. | Redis location store |
| [SupportChat](SupportChat) | API, Support, and Session roles demonstrate conversation ownership, reconnect, idle timeout, and close notifications. | Redis location store |
| [ShoppingMall](ShoppingMall) | Commerce API and order workflow roles demonstrate event-sourced orders, projections, and fanout events. | Redis location store |
| [DeliveryDispatch](DeliveryDispatch) | Dispatch, courier, tracking, and customer gateway roles demonstrate timeout reassignment and session push. | Redis location store |
| [GameQuest](GameQuest) | Session and player quest owner roles demonstrate event-sourced quest progress and projections. | Redis location store |
| [ZoneWorld](ZoneWorld) | Gateway, ZoneNode, and Ops roles demonstrate Actor transfer, zone Logical Multicast, Node direct operations, runtime events, and browser visualization. | Redis location store |

TicTacToe is the only sample that configures MeshNode peers manually. Every
other sample uses the Redis location store to resolve Spot and Actor locations
and establish MeshNode peers.

## MeshNode And Channel Names

Each physical mesh has one MeshNode per process. `ChannelName(...)` adds logical
service membership to that MeshNode and does not create another ROUTER endpoint.
Node direct, ChannelName select-one, Spot, Actor, and Logical Multicast operations
share the MeshNode. Classic fanout remains a separate PUB/SUB channel.

```csharp
var mesh = options.AddRouteMesh("game")
    .Listen("tcp://0.0.0.0:7300"); // Creates this process's MeshNode endpoint.

mesh.ChannelName("orders"); // Adds logical service membership without another ROUTER.

options.AddFanoutChannel("events")
    .EnablePublisher("tcp://0.0.0.0:7400"); // Classic fanout uses its own PUB endpoint.
```

## Running Samples

Run all supported samples on Linux or WSL:

```bash
./framework/languages/dotnet/samples/run_samples.sh
```

Run all supported samples on Windows PowerShell:

```powershell
.\framework\languages\dotnet\samples\run_samples.ps1
```

Pass sample names to run a subset in the given order.

```bash
./framework/languages/dotnet/samples/run_samples.sh Bingo SupportChat
```

Each sample root owns `run_sample.sh` and `run_sample.ps1`. The runner creates
role-specific configuration files, starts each role as a separate process,
waits for readiness, runs the probe or client self-check, and then removes the
processes and Redis container it created. Server code starts only its own role.

## Configuration And Contracts

Framework hosts bind endpoint, Redis, routing ID, timeout, and logging settings
from role-specific configuration files and pass typed settings to
`AddZLinkFramework(...)`. Application code does not read those values directly
from environment variables. A standalone client accepts only the external
endpoint and scenario options it must know through validated command-line
arguments or its own configuration file.

Shared projects contain only message contracts serialized by both client and
server roles. Server topology and framework settings belong under
`Server/Configuration`; client and probe settings belong to their respective
projects.

For a manual TicTacToe run, give each role its own configuration file:

```bash
dotnet run --project TicTacToe/Server.Play -- --config ./appsettings.play-a.json
dotnet run --project TicTacToe/Server.Play -- --config ./appsettings.play-b.json
dotnet run --project TicTacToe/Server.Api -- --config ./appsettings.api-a.json
dotnet run --project TicTacToe/Server.Api -- --config ./appsettings.api-b.json
```
