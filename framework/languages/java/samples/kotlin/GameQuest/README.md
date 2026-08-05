# GameQuest Kotlin

Kotlin version of the GameQuest player quest sample.

The sample splits the game edge and quest owner roles into separate processes.
`GameApi` owns stream sessions, validates gameplay actions, records gameplay
facts, and routes each player event directly to its `PlayerQuestSpot` Instance Spot.
`QuestMission` evaluates quest progress, records domain events, rebuilds
projections, and returns progress notifications that `GameApi` pushes back over
the bound stream session.

## Run

Run the complete sample scenario on Linux or WSL:

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
`GAMEQUEST_REDIS_KEY_PREFIX` for each execution so parallel
sample runs do not share location-store or sample-state keys.

## Layout

- `Shared/` contains gameplay action, quest progress, notification, and
  self-check contracts.
- `Client/` contains the self-checking two-player stream scenario.
- `Server/GameApi/` owns stream sessions, gameplay validation, owner routing,
  projection self-check endpoints, and client push.
- `Server/QuestMission/` owns quest event processing, projection rebuild, and
  sync/reconcile handling.
- `Server/Configuration/` contains endpoint, channel, Redis, location-store,
  timing, and marker settings.

The successful run prints `gamequest-server-evidence=completed` from the client
after server evidence is checked and `gamequest kotlin full client/server
self-check completed` from the runner.
