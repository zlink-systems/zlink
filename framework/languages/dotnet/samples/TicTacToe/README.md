# Tic Tac Toe Sample

This sample maps a scale-out tic-tac-toe flow onto `Zlink.Framework`:

1. the runner starts two API roles, `api-a` and `api-b`,
2. the runner starts two Play roles, `play-a` and `play-b`,
3. each API role has one MeshNode with manually configured Play peer endpoints,
4. each Play role has one MeshNode, and the second Play role connects to the first,
5. the client calls an API role over HTTP to create a room,
6. the API role calls `IZLinkSpotManager.Create` with the initial game settings,
7. the framework issues the room id, selects an eligible Play node, and records the
   room location in the configured Location Store,
8. the API response returns the room id, `PlayEndpoints`, and `PlayNodes`,
9. the host stream client connects to the first Play endpoint,
10. the guest and observer stream clients connect to the other Play endpoint,
11. each stream client sends `AuthenticateReq` to the Play server,
12. the Play session asks an API role to authenticate over the independent
    `tictactoe.api` ClientServer channel,
13. the host and guest actors join the same tic-tac-toe room,
14. the observer actor subscribes to milestone notifications through `ObserveMilestoneReq`,
15. the actors send `PlaceMarkReq` packets until player X wins,
16. the room pushes `PlayerJoinedNotify` and `GameStateNotify`,
17. the owner room publishes the win milestone through the Play RouteMesh channel
    and the observer receives `WinMilestoneNotify`,
18. the host and guest send `LeaveGameMsg`, and the server destroys both entry-spot actors.

Packet type names use `Req` for request packets, `Res` for response packets,
and `Notify` for server push packets.

TicTacToe uses JSON payloads for STREAM, channel, actor, and room Spot
messages.

The sample is grouped by its own solution:

```bash
dotnet build framework/languages/dotnet/samples/TicTacToe/TicTacToe.sln
```

Run the sample smoke path:

```bash
framework/languages/dotnet/samples/TicTacToe/run_sample.sh
```

The standalone client lives in [`Client`](Client). Use it when you want to
read or run just the client side of the flow. `Program` reads the client options
and runs `TicTacToeClientScenario`; the scenario calls HTTP `POST /games`, reads
the returned `PlayEndpoints` and `PlayNodes`, creates host, guest, and observer
stream connectors, then verifies authentication, joins, moves, regular pushes,
observer milestone push, and leave cleanup.
The request, response, and push DTOs live in [`Shared`](Shared) so the
server and client use the same protocol contract. The reusable client flow lives
in [`Client`](Client); the sample runner starts the server roles and then
runs that client as the self-check.

The API and Play roles use separate executable projects. Each process receives
only its role-specific configuration file:

```bash
dotnet run --project framework/languages/dotnet/samples/TicTacToe/Server/Play -- --config ./appsettings.play-a.json
dotnet run --project framework/languages/dotnet/samples/TicTacToe/Server/Play -- --config ./appsettings.play-b.json
dotnet run --project framework/languages/dotnet/samples/TicTacToe/Server/Api -- --config ./appsettings.api-a.json
dotnet run --project framework/languages/dotnet/samples/TicTacToe/Server/Api -- --config ./appsettings.api-b.json
dotnet run --project framework/languages/dotnet/samples/TicTacToe/Client
```

Each role reads `Sample` settings from the config file through
`Microsoft.Extensions.Configuration`. The runner writes temporary role-specific
settings, starts `play-a`, `play-b`, `api-a`, and `api-b` with `--config`, waits
for stream, MeshNode, HTTP, and Redis endpoints, and
then runs the standalone client. The runner fails if the logs do not contain
stream-inbound response and push evidence, observer milestone verification,
`LeaveGameMsg` completion for both players, entry-spot actor destroy evidence
for both players. Framework diagnostics use the standard .NET
`ActivitySource` and `Meter` surfaces instead of a sample-owned trace file.

Redis is required as the sample's official Location Store provider. The runner always provisions a
dedicated Redis Docker container for the current execution, asks Docker to
assign a free loopback host port, derives `TICTACTOE_REDIS_ENDPOINT` from that
container, and removes only that container on success or failure. It does not
reuse an externally supplied Redis endpoint. The runner also supplies a
`TICTACTOE_REDIS_KEY_PREFIX` that includes the sample name and execution id so
parallel sample runs do not share Location Store keys.
