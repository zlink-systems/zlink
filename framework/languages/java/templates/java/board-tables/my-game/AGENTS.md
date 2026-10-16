# MyGame board tables instructions

This template implements Java Session and Table processes, a shared typed contract module, and a Java e2e runner for board-table games.

## Node responsibilities

| Module | Responsibility and extension point |
| --- | --- |
| `server/session` | STREAM termination, Actor creation/binding and relay in `GameSession`. |
| `server/table` | Entry admission in `Join`, game rules and state in `Table`, identity in `Player`, and application transfer in `TableTransfer`/`PlayerTransfer`. Run this project once per Table node. |
| `server/shared` | Typed messages, game constants, configuration, Redis stores and host control in `Hosting`. |
| `e2e` | Structure validation, owned infrastructure/process cleanup and received-message assertions. |

## Public API boundaries

Use public Framework and stream connector APIs and the default typed JSON serializer. Reply to the requesting Actor through the handler's bound Session. Send other pushes by the playerId owned by Table state through Actor send; the typed forwarding handlers deliver them to the bound Session. Store no Actor references in Table state.

Table owns game state. Player owns identity and connection access through Framework context. Relocation adapters transfer application state only. Framework owns routing, membership, leases, queues and execution gates. Do not add internal APIs, reflection, raw frame parsing, manual codecs, sleeps, retries, larger deadlines or weakened assertions to conceal a Framework failure.

## Changes and verification

Keep the structure, configuration IDs, server constants, README commands and scenario list consistent. Validate actual files with the structure checker. Maintain `table`, `reconnect` and `update`; received responses and events determine success. An intermittent failure is a defect.

Run build and e2e on Windows and Linux/WSL. Preserve the first failure and node message-flow records from `.logs/run-<id>/`. Clean up only infrastructure and processes owned by the current run. Keep Framework versions centralized in `build.gradle.kts` and never commit local feeds or repository-source dependencies.
