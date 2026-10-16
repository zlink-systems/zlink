# Project instructions

This C++ template implements an action/FPS entry lobby with mode-based matchmaking and authoritative two-player Rooms. The structure descriptor and e2e scenarios are part of its runnable contract.

## Node responsibilities

| Role | Responsibility and extension point |
| --- | --- |
| Session | `server/session/`: accept STREAM, get or create and bind Player Actors, answer identity requests, relay packets. |
| Match | `server/match/`: Entry routes queue requests; Matchmaking owns queues and Room numbering; Room owns combat state. Add game rules in `domain.hpp` and Framework handlers in `spots.hpp`. |
| Shared hosting | `server/shared/`: configuration, typed contracts, Redis stores, diagnostics and stdin lifecycle protocol. |
| E2E | `e2e/`: public connector clients, process ownership, structural consistency and gameplay assertions. |

## Public API boundaries

- Use only published Framework and connector APIs and the default typed JSON serializer. Do not add packet-specific codecs, internal API access, raw-frame parsing or repository-source dependencies.
- A request handler sends its response directly through the received Actor's bound Session. Other pushes target player IDs through the public Actor client and are forwarded by the current Spot's Actor handler. Spots never retain Actor references.
- Matchmaking and Room each own their application state. Player has no game state. Relocation adapters capture only application state, never membership, leases, routing, queues owned by Framework or binding state.
- C++ SessionInfo is a typed push because #1785 lacks a public typed reply API. Do not manufacture a reply with manual encoding.
- Execution gates, completion, reconnect, handoff and placement belong to Framework. Do not compensate with locks, sleeps, retries, larger deadlines or relaxed assertions.

## Changes and verification

Keep `hit`, `reconnect`, `update` in that order in the descriptor and runner. Keep README commands equal to `run.commands`. Run the domain tests and structure check before e2e, then validate on Windows and Linux. Any intermittent failure remains a defect; a subsequent pass does not cancel it. Preserve first-failure node message-flow and client logs in `.logs/run-<id>/`. Framework defects belong in the job report with reproduction, first log evidence and source locations; do not fix Framework from this template.

Run formatters and installation commands only from this template directory. Preserve English prose here and in README, except README's final Korean section. Code comments are English.
