# Tic Tac Toe Client

This is the standalone sample client for `TicTacToe`.

Start the server roles first:

```bash
Server/build/install/Server/bin/tictactoe-play --config ./application.properties
Server/build/install/Server/bin/Server --config ./application.properties
```

Then run the client:

```bash
gradle :Client:run
```

Options:

```bash
gradle :Client:run --args='--api-url http://127.0.0.1:18081 --game-name tictactoe-game --x-actor-id player-x --o-actor-id player-o'
```

Each actor id is sent as the sample authentication token. The client calls the
API role over HTTP, opens two STREAM connections to the Play role, authenticates
both players, joins them to one game, and plays a fixed five-move sequence where
X wins. It registers typed handlers for `GameStateNotify` and
`PlayerJoinedNotify` and prints the collected notification counts.
