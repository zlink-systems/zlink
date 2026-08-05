# GameQuest Java

This sample implements the common GameQuest workflow with separate `GameApi`
and `QuestMission` processes. `GameApi` owns stream sessions, validates gameplay
actions, records gameplay facts, and routes player events to the quest owner.
`QuestMission` evaluates progress, appends quest events, rebuilds projections,
and returns notifications for the bound client session.

## Run

Run the complete Java scenario on Linux or WSL:

```bash
./run_sample.sh
```

The runner provisions a dedicated Redis Docker container on a loopback port and
removes it on success or failure. It does not reuse an external Redis endpoint.
Each run also receives a unique key prefix so location and sample-state keys do
not overlap another run.

## Layout

- `Shared/` contains gameplay, quest progress, notification, and self-check contracts.
- `Client/` contains the self-checking two-player stream scenario.
- `Server/GameApi/` owns sessions, gameplay validation, routing, and client push.
- `Server/QuestMission/` owns quest events, projection rebuild, and reconciliation.
- `Server/Configuration/` contains endpoint, ChannelName, Redis, timing, and marker settings.

The common GameQuest sample document owns the workflow and message contract.
This README records only the Java layout and execution boundary.
