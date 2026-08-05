# Tic Tac Toe Client

This is the standalone sample client for `TicTacToe`.

Start the two Play roles and two API roles first:

```bash
framework/languages/dotnet/samples/TicTacToe/run_sample.sh
```

On Windows PowerShell:

```powershell
.\framework\languages\dotnet\samples\TicTacToe\run_sample.ps1
```

The runner starts `play-a`, `play-b`, `api-a`, and `api-b`, waits for their
stream, MeshNode, HTTP, and Redis endpoints, runs this client, and stops the
servers. The API creates the game Spot through `IZLinkSpotManager`; the
framework selects its Play owner. To run the client against already running
roles:

```bash
dotnet run --project framework/languages/dotnet/samples/TicTacToe/Client
```

Options:

```bash
dotnet run --project framework/languages/dotnet/samples/TicTacToe/Client -- \
  --api-url http://127.0.0.1:18080 \
  --game-name tictactoe-game \
  --x-actor-id player-x \
  --o-actor-id player-o \
  --observer-actor-id observer
```

Each actor id is sent as the sample authentication token. The API server
returns that value as `actorId`, and the play server uses it as the actor
`ActorId`. The sample client opens three STREAM connections: host, guest, and
observer. The host connects to the first Play endpoint returned by
`POST /games`; the guest and observer connect to the second endpoint. Actor and
Spot routing does not depend on which stream endpoint accepted the session.
The observer sends `ObserveMilestoneReq` before the game starts.

The client joins the host and guest actors to one game, receives
`PlayerJoinedNotify` and `GameStateNotify` push packets, then plays a fixed
five-move sequence where X wins. After the final state, the observer waits for
`WinMilestoneNotify`, and the host and guest send `LeaveGameMsg` so the server
can leave and destroy both entry-spot actors. The game SPOT also owns a timer
that ends the game with `TurnTimedOut` when the current player takes too long.
