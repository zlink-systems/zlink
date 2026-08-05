# TicTacToe Kotlin

Kotlin version of the TicTacToe framework sample.

The Kotlin TicTacToe client and server roles use JSON payloads for STREAM,
channel, actor, and room Spot messages.
The client verification flow lives in `TicTacToeClientScenario`.

The sample is split into standalone Spring role projects:

- `Client`: sends an HTTP `CreateGameHttpReq` to the API role, opens two STREAM
  connections to the Play role, authenticates both players, joins one game, and
  plays a fixed winning sequence. It also registers typed STREAM handlers for
  `GameStateNotify` and `PlayerJoinedNotify`.
- `Server`: starts one Spring Boot role per process. Use `play` or `api` to run
  a role. The API role exposes the `/games` HTTP endpoint plus
  `AuthenticatePlayer` channel handler. The Play role owns the STREAM endpoint,
  actor runtime, entry Spot, game Spot, and Redis-backed framework location
  store used for remote Spot routing.
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

The runner always provisions a dedicated Redis Docker container on a
Docker-assigned loopback port and removes that container on success or failure.
It does not reuse an external Redis endpoint. The runner also supplies a unique
Redis key prefix for each execution so parallel sample runs do not share
location-store keys.

Run the roles manually:

```bash
gradle :Server:installDist
Server/build/install/Server/bin/tictactoe-play --config ./application.properties
Server/build/install/Server/bin/Server --config ./application.properties
gradle :Client:run
```
