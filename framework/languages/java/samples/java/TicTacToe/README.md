# TicTacToe

Direct STREAM, Spot, and channel-flow sample check.

The Java TicTacToe client and server roles use JSON payloads for STREAM,
channel, actor, and room Spot messages.
The client verification flow lives in `TicTacToeClientScenario`.

The sample is split into standalone Spring role projects:

- `Client`: sends an HTTP `CreateGameHttpReq` to the API role, opens two STREAM
  connections to the Play role, authenticates both players, joins one room, and
  plays a fixed winning sequence. It also registers typed STREAM handlers for
  `GameStateNotify` and `PlayerJoinedNotify`.
- `Server`: starts one Spring Boot role per process. Use `play` or `api` to run
  a role. The API role exposes the `/games` HTTP endpoint plus
  `AuthenticatePlayer` channel handler. The Play role owns the STREAM endpoint,
  actor runtime, entry Spot, game Spot, and Redis-backed framework location
  store used for remote Spot routing. This sample keeps only the common spec
  flow.
- `Shared`: holds the message contracts used by the client, API role, Play
  role, and STREAM messages.

Run the standalone role sample check:

```bash
./run_sample.sh
```

On Windows:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\run_sample.ps1
```

Run the roles manually:

```bash
gradle :Server:installDist
Server/build/install/Server/bin/tictactoe-play --config ./application.properties
Server/build/install/Server/bin/Server --config ./application.properties
gradle :Client:run
```
