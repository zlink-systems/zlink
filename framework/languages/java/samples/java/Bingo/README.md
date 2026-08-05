# Bingo

Two-client Session/API/Play sample check with Redis-backed location store.

The client opens only the Session stream endpoint. Each player authenticates,
requests matching, receives game-start push after the second join, submits a
3 x 3 card, and waits for server timer draw and game-ended notifications. The
client never sends draw requests.

The Java Bingo client and server roles use Protobuf payloads for STREAM
messages. Shared contains only the message contracts used by those roles.
The client verification flow lives in `BingoClientScenario`.

Run the complete sample scenario on Linux or WSL:

```bash
./run_sample.sh
```

On Windows:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\run_sample.ps1
```

The runner always provisions a dedicated Redis Docker container on a
Docker-assigned loopback port, derives `BINGO_REDIS_ENDPOINT` from it, and
removes that container on success or failure. It does not reuse an external
Redis endpoint. The runner also supplies a unique
`BINGO_REDIS_KEY_PREFIX` for each execution so parallel sample runs do not share
Redis match queue keys.
