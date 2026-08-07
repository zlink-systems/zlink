# Tic Tac Toe Client

This is the standalone sample client for `TicTacToe`.

Run the complete sample from the TicTacToe project root:

```bash
./run_sample.sh
```

The runner creates the endpoint and Redis configuration for the server roles,
starts them, runs this client, checks the result, and removes the resources it
created. There is no checked-in `application.properties` for an independent
role launch.

Options:

```bash
../../gradlew --settings-file standalone.settings.gradle.kts :Client:run --args='--api-url http://127.0.0.1:18080 --game-name tictactoe-game --x-actor-id player-x --o-actor-id player-o'
```

The options command assumes that the server roles were started with matching
settings; the runner command above is the supported self-contained execution.

Each actor id is sent as the sample authentication token. The client calls the
API role over HTTP, opens two STREAM connections to the Play role, authenticates
both players, joins them to one game, and plays a fixed five-move sequence where
X wins. It registers typed handlers for `GameStateNotify` and
`PlayerJoinedNotify` and prints the collected notification counts.
