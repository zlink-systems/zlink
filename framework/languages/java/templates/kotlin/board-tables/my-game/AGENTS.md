# MyGame agent guidelines

This project is a runnable Kotlin server starting point for four-player board tables. `zlink.structure.json` describes the initial composition.

## Node responsibilities

| Module | Responsibility |
| --- | --- |
| `server/shared` | Typed contracts, game constants, node configuration, the public bootstrap, and the control protocol. |
| `server/session` | STREAM termination, Player GetOrCreate and bind, and relay. |
| `server/table` | Entry Spot, Table User Spot, seats and game state, and the relocation adapter. |
| `e2e` | Public connector client, judging by actual responses, process and Compose lifetime, and the structure check. |

## Extension points

Extend game rules in `Table.kt`, messages in `Contracts.kt`, and public registration in each node's `Program.kt`. Check the IDs in the configuration together with `zlink.structure.json`. Reuse the constants in `Names` and `Rules`. The Initializr replaces `my-game` and `MyGame`, so do not introduce other forms of the project name.

## Public API boundaries

Use only the public Framework and binding packages. Keep the default typed JSON serializer. Do not copy Table state into the Player Actor, and do not put Framework route, lease, membership, or queue state into the relocation adapter. The SPOT_WIDE gate serializes game state. Do not compensate for a runtime defect with a separate lock, queue, retry, or sleep.

`SessionInfo` owns the sessionId and the bound Actor's actorId and objectGeneration; `TableView` carries only the table state. A request handler sends its response directly to the requesting Actor's bound session; other state pushes use the public Actor send API.

## Verification

Run `./run-e2e.sh` or `pwsh -NoProfile -File ./run-e2e.ps1`. The default scenarios are `table`, `reconnect`, and `update`. Judge by responses and runtime observations, and do not add fixed waits. The structure check validates the actual files at the start of every run. Verify on a temporary copy during development that bad structure input is rejected, and do not leave self-test code in the template.

Keep failure logs and the first message flow error. Do not cover an intermittent failure with a successful rerun. Limit cleanup to the processes and Compose project the run created. For a defect in a public package, report the reproduction, the first failure log, and the relevant source location, and do not work around it in the template. Write code comments in English and technical reports in formal Korean.
