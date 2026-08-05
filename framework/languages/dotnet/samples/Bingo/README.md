# Bingo Sample

This sample implements the common Bingo flow from
`framework/doc/framework/common/sample/bingo/README.ko.md`.

Bingo is the Protobuf game sample. Its stream, channel, actor, and room Spot
payloads use the framework Protobuf codec so the multi-server game flow also
shows a schema-oriented binary contract.

Directory layout:

- `Shared/` contains only the public sample DTO and protobuf contracts.
- `Client/` contains the real stream connector client. The scenario creates
  three client connectors: `player-1` connects to Session A, while `player-2`
  and `observer` connect to Session B.
- `Client/Configuration/` contains client-only endpoint and packet settings.
- `Server/Configuration/` contains the server topology, packet names, and
  framework timing settings used by the server roles.
- `Server/Api/` contains player authentication, matching, and in-memory player
  record handlers.
- `Server/Matchmaking/` contains the level-bucket Matchmaker Instance Spot and
  the Redis reservation adapter.
- `Server/Play/` contains player actors, Entry Spot admission, room spots,
  submitted cards, server-driven draws, automatic
  marks, winner detection, rare reward Logical Multicast to subscribed Spots, and
  session-bound push.
- `Server/Session/` contains the stream Session server and actor relay handlers.
- Servers register themselves in a shared location store (the per-run Redis
  container) and auto-connect; no registry process exists.

Each top-level client/shared directory and each server role directory has its
own project file. The root `Bingo.csproj` is the aggregate build entry point for
IDEs and CLI builds. `Bingo.sln` keeps the same layout: `Shared`, `Client`,
`Server/Configuration`, and `Server/<role>`.

Build the whole sample with:

```bash
dotnet build "framework/languages/dotnet/samples/Bingo/Bingo.csproj"
```

Run it with:

```bash
framework/languages/dotnet/samples/Bingo/run_sample.sh
```

On Windows PowerShell:

```powershell
.\framework\languages\dotnet\samples\Bingo\run_sample.ps1
```

The script starts two Api servers, one Matchmaking server, two Play servers,
two Session servers, and Redis-backed store access as separate processes. It waits for the
public endpoints that the client and server roles use, and then runs the
connector client. The
client flow is self-checking. It fails if the three connectors do not
authenticate as distinct actors, match into one room across Play nodes, observe
the rare reward event from the non-owner Play node, or deliver push
notifications to the bound client sessions. The room loads each player's win
and loss record through an explicit yielding request before adding that player
to the game domain. After the game finishes, it reports each result through the
same execution policy. The server self-check verifies those two record loads
and two result reports before confirming that room actors leave the room Spot,
return to Entry Spot, and are destroyed from the Entry Spot context.

Session and Play also attach the standard .NET `MeterListener` to
the standard .NET meter named `zlink.framework`. The runner verifies real STREAM and Spot samples, while
Play uses the fixed MeshNode drain sequence. It stops new application admission,
finishes accepted work, completes Actor handoff and the STREAM barrier, and then
closes the remaining local rooms before releasing location ownership.

Redis is required for two responsibilities: framework location store data and
the Matchmaking reservation store. The runner always provisions a dedicated Redis Docker
container for the current execution, asks Docker to assign a free loopback host
port, writes that endpoint to each role's temporary config file, and removes only that
container on success or failure. It does not reuse an externally supplied Redis
container. The runner also writes a Redis key prefix that includes the
sample name and execution id so parallel sample runs do not share location store
or reservation keys.
