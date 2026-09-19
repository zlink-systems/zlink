# Node/TypeScript Tutorial

A program that the feature guides read through, chapter by chapter. This directory carries over
**Channel messaging and one id-addressed Spot** from
[`../../dotnet/tutorial/`](../../dotnet/tutorial/). It does not cover Actor or STREAM.

## Why this is separate from quickstart and the samples

| | Purpose |
|---|---|
| [`../quickstart/`](../quickstart/) | From install to the first response. Adds no features |
| **`tutorial/`** (here) | Builds up features one at a time. The feature guides read this code |
| [`../samples/`](../samples/) | Applications that show a complete business flow |

## Prerequisites

- **Node.js 22 or newer.** `@zlink-systems/zlink` declares `"engines": { "node": ">=22" }`.
  Check with `node --version`.
- **Docker Desktop (or Docker Engine) running.** It hosts a single Redis (see "Run" below).
  Nothing else needs to be installed.
- **Windows installs without a native build starting from framework 0.18.1.**
  `@zlink-systems/zlink` 1.2.1 ships `prebuilds/linux-x64/` and `prebuilds/win32-x64/` together
  (#656). `@zlink-systems/framework` pins that version exactly starting at 0.18.1 — a zip
  pinning an earlier framework still gets `1.2.0` only, so `npm install` on Windows falls back
  to `node-gyp rebuild`, which requires `ZLINK_CORE_INSTALL_PREFIX` pointing at an installed
  Core, and fails. macOS (`darwin-*`) has no prebuild yet. This document's output was captured
  on Windows 11 under WSL2 Ubuntu-24.04, Node `v22.23.2`, npm `10.9.8`.

## Download and install

This directory is already the download — unzip `zlink-tutorial-node.zip` and it opens straight
into this tutorial. There is no repository to clone. Like quickstart, it references only npm
registry packages.

```bash
npm install
```

| Package | Pinned version | Why |
|---|---|---|
| `@zlink-systems/framework` | `0.18.0` | `npm view @zlink-systems/framework versions` → `0.10.0` through `0.18.0` |
| `@zlink-systems/framework-locations-redis` | `0.18.0` | Same list. The Location Store/Relocation Store implementation the Spot stage needs |
| `@zlink-systems/nestjs` | `0.18.0` | Same list. Depends on `@zlink-systems/framework: '0.18.0'` exactly |
| `@nestjs/common`/`@nestjs/core` | `10.4.22` | `@zlink-systems/nestjs@0.18.0` depends on and peer-depends on `^10.4.22` |
| `reflect-metadata` | `0.2.2` | Satisfies `@zlink-systems/nestjs@0.18.0`'s `^0.2.2` range |
| `@zlink-systems/zlink` | Not pinned | `@zlink-systems/framework` pins it exactly. 0.18.0 pins `1.2.0`; starting at 0.18.1 it pins `1.2.1`, which ships the win32-x64 prebuild (#656). Left to transitive resolution |

The three `@zlink-systems` package versions are updated by the repository's
`scripts/local-package/sync-version.py` to match `framework/languages/node/VERSION` (repository
only). They are never hand-edited.

## Build

```bash title="linux"
npm run build
```

```powershell title="windows"
npm run build
```

`StreamClient` is a separate project with its own `package.json`. The connector ships as ESM
only while the tutorial itself is CommonJS, so they are not built under one tsconfig — it is
built separately (see "10. STREAM and the Session-Actor Link" below).

## Run

Redis is required (the Spot stage uses it). There is no runner here, so this tutorial starts one
itself — and you clean it up yourself when done. Following along in two terminals, you can just
run `npm run server` then `npm run client` directly. The block below does the same thing
unattended, backgrounding both and leaving their PID in a file.

```bash title="linux"
docker run -d --rm --name zlink-tutorial-node-redis -p 127.0.0.1:6379:6379 redis:7.2-alpine
sleep 1
npm run server > server.log 2>&1 &
echo $! > server.pid
npm run client > client.log 2>&1 &
echo $! > client.pid
```

```powershell title="windows"
docker run -d --rm --name zlink-tutorial-node-redis -p 127.0.0.1:6379:6379 redis:7.2-alpine
Start-Sleep -Seconds 1
$serverProc = Start-Process -PassThru -NoNewWindow npm.cmd -ArgumentList 'run','server' -RedirectStandardOutput server.log -RedirectStandardError server.err.log
Set-Content -Path server.pid -Value $serverProc.Id
$clientProc = Start-Process -PassThru -NoNewWindow npm.cmd -ArgumentList 'run','client' -RedirectStandardOutput client.log -RedirectStandardError client.err.log
Set-Content -Path client.pid -Value $clientProc.Id
```

The full feature set is confirmed one step at a time below, under "Steps".

## Verify

The server and client each print a line like this on a healthy start.

```
server listening on tcp://0.0.0.0:7701 (mesh "game", routing id "game-server-1")
server admin listening on http://127.0.0.1:5481
```

```
client listening on http://127.0.0.1:5480
```

Startup can take up to 45 seconds (especially on a WSL 9p mount like `/mnt/d` — see
"Troubleshooting" below), so the confirming call retries for up to 60 seconds instead of a
fixed wait. It always cleans up both processes and the Redis container regardless of outcome,
and only exits non-zero when the check never succeeded.

```bash title="linux"
ready=""
for _ in $(seq 1 60); do
  if curl -sf http://127.0.0.1:5480/players/p1/profile > response.json 2>/dev/null; then
    ready=1
    break
  fi
  sleep 1
done
[ -n "$ready" ] && cat response.json
kill "$(cat client.pid)" "$(cat server.pid)" 2>/dev/null
docker rm -f zlink-tutorial-node-redis
[ -n "$ready" ]
```

```powershell title="windows"
$ready = $false
for ($i = 0; $i -lt 60; $i++) {
    try {
        $response = Invoke-RestMethod http://127.0.0.1:5480/players/p1/profile -ErrorAction Stop
        $ready = $true
        break
    } catch {
        Start-Sleep -Seconds 1
    }
}
if ($ready) { $response | ConvertTo-Json -Compress }
taskkill /F /T /PID $(Get-Content client.pid) 2>$null
taskkill /F /T /PID $(Get-Content server.pid) 2>$null
docker rm -f zlink-tutorial-node-redis
if (-not $ready) { exit 1 }
```

On success it returns:

```json
{"playerId":"p1","nickname":"rookie","level":1}
```

Seeing this means mesh/channel registration and the RouteMesh call path are all working. Expected
responses for each step are under "Steps" and "Actual output" below.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `docker: Cannot connect to the Docker daemon` | Docker Desktop (or `dockerd`) is not running. Start it and try again |
| `curl` returns `Connection refused` | The server (`npm run server`) is not up yet or has died. Check that terminal's log first |
| `EADDRINUSE` (port conflict) | Another process already holds one of the ports in "Ports" below. Stop it, or clean up another running instance of this tutorial first |
| `npm error gyp ERR! ... ZLINK_CORE_INSTALL_PREFIX must name an absolute installed Core ... package prefix` (Windows) | This zip pins a `@zlink-systems/framework` older than 0.18.1, so it still gets `zlink@1.2.0` — the win32-x64 prebuild (#656) ships from framework 0.18.1 (`zlink@1.2.1`) on. Get that version, or run it under WSL |
| Same error (macOS) | `@zlink-systems/zlink@1.2.1` also has no `darwin-*` prebuild yet. Run it on Linux (x64) or Windows (0.18.1 on) |
| `EBADENGINE` (Node version warning) | Node.js is older than 22. Upgrade per "Prerequisites" above |
| `server listening` takes 20-45 seconds to appear | The project sits on a WSL 9p mount such as `/mnt/d`; module loading alone takes this long there. Moving it to a Linux filesystem (e.g. `~/`) cuts this down |

## Ports

| Use | Value |
|---|---|
| Client's HTTP | `127.0.0.1:5480` |
| Server's admin HTTP | `127.0.0.1:5481` |
| Server's mesh listen | `0.0.0.0:7701` (advertises `127.0.0.1`) |
| Client's mesh listen | `0.0.0.0:7702` (advertises `127.0.0.1`) |
| ClientServer channel | `127.0.0.1:7711` |
| Fanout publisher | `127.0.0.1:7712` |
| Server's stream listen | `0.0.0.0:7721` (WebSocket) |

## Project layout

| Path | Role |
|---|---|
| `Shared/contracts.ts` | Message contracts, packet names, mesh/channel names shared by both sides |
| `Server/main.ts` | Registers mesh, channel, node-direct, ClientServer and Fanout. Opens one HTTP endpoint just for runtime weight |
| `Server/Channel/` | Channel, ClientServer and Fanout handlers |
| `Server/Ops/` | Node-direct call handlers |
| `Server/Spots/game-room.ts` | One id-addressed Spot and its two handlers |
| `Server/Dispatch/` | The filter that wraps every handler |
| `Client/main.ts` | Takes HTTP and calls out over mesh, ClientServer and Fanout |
| `Client/zlink-error-response.ts` | Maps a Framework exception's error kind to an HTTP status and body |
| `Server/Actors/player.ts` | One id-addressed Actor and its two handlers |
| `Server/Spots/lobby-spot.ts` | The Entry Spot a newly created player first enters |
| `Server/Sessions/` | The session owning one external client connection, and its two handlers |
| `StreamClient/` | The client outside the mesh. A **separate project** that references only the connector, not the framework |

## Steps

### 1. Channel messaging — RouteMesh

The caller does not pick a node. Give only the channel name, and the node responsible for that
channel receives it.

```bash
curl http://127.0.0.1:5480/players/p1/profile
# {"playerId":"p1","nickname":"rookie","level":1}

curl -X POST http://127.0.0.1:5480/players/p1/logins
# 202. Server log: login recorded: p1
```

The second call is one-way and does not wait for a response.

### 2. Channel messaging — direct node calls

A path that bypasses channels. The receiver registers straight on the mesh with
`mesh.addRequestHandler`, and the caller names the node's routing id. Used only for operational
commands.

```bash
curl http://127.0.0.1:5480/ops/nodes/game-server-1/status
# {"meshName":"game","channelName":"(none)",
#  "calledBy":"game-388cbafe-f139-4e61-9c0a-361168e3e822","uptime":"64s","processId":3128502}

curl -i http://127.0.0.1:5480/ops/nodes/no-such-node/status
# 404 {"error":"not_found","message":"MeshNode 'game' request failed with result 102 and errno 14."}
# Unlike a channel call, no candidate is picked, so this simply fails.
```

The point is that `channelName` is empty — a channel was never involved. `calledBy` is the
calling node's routing id. The client never pins a routing id, so it shows the
`game-<uuid>` shape the framework generated. That is why the receiving node pins its id with
`routingId('game-server-1')`.

### 3. Channel messaging — ClientServer

The call code looks almost the same as above. What differs is **who receives it**: the server the
caller is connected to.

```bash
curl -X POST http://127.0.0.1:5480/players/p1/tickets
# "ticket-p1"
```

### 4. Channel messaging — Fanout

The sender does not know which node will receive it. Every subscribed node does.

```bash
curl -X POST http://127.0.0.1:5480/notices \
  -H 'Content-Type: application/json' -d '{"message":"scheduled maintenance"}'
# 202. Server log: maintenance notice: scheduled maintenance
```

### 5. Filter

One `Server/Dispatch/call-log-filter.ts` wraps all four paths above. Sending the calls above in
order produces this server log:

```
LOG [CallLogFilter] dispatch start: GetPlayerProfile
LOG [CallLogFilter] dispatch done: GetPlayerProfile in 1ms
LOG [CallLogFilter] dispatch start: RecordLogin
LOG [RecordLoginHandler] login recorded: p1
LOG [CallLogFilter] dispatch done: RecordLogin in 1ms
LOG [CallLogFilter] dispatch start: IssueSessionTicket
LOG [CallLogFilter] dispatch done: IssueSessionTicket in 1ms
LOG [CallLogFilter] dispatch start: MaintenanceNotice
LOG [MaintenanceNoticeSubscriber] maintenance notice: scheduled maintenance
LOG [CallLogFilter] dispatch done: MaintenanceNotice in 1ms
LOG [CallLogFilter] dispatch start: GetNodeStatus
LOG [CallLogFilter] dispatch done: GetNodeStatus in 0ms
```

The filter wraps even the Fanout subscription handler (`MaintenanceNotice`).

### 6. Changing weight at runtime

Channel weight is a value you can change while this node is running. Set it to 0 and the socket
stays open and in-flight calls finish, but no other node picks this node as a target for new
calls. 100 is the usual value.

The server opens HTTP on `127.0.0.1:5481` just for this one endpoint. Since the call carries no
body, the new value is given as a query string.

```bash
curl -X POST 'http://127.0.0.1:5481/admin/channels/profile/weight?value=0'
# {"channel":"profile","weight":0}

curl -X POST 'http://127.0.0.1:5481/admin/channels/profile/weight?value=100'
# {"channel":"profile","weight":100}
```

The point is what stops while weight is 0. Only calls 1 and 2, which go through the `profile`
channel, fail to find a target; the direct node call, ClientServer and Fanout still get 200 and
202, because none of the three go through channel candidate selection. "Actual output" below has
a full run of all three.

Both failing calls end as `Unavailable` and return 503. `Client/zlink-error-response.ts` is what
decides the status code — it maps the error kind of the Framework's `ZLinkFrameworkException`
exception to an HTTP status, and without it both would be 500, leaving the caller unable to tell
"no node can receive this right now" apart from "the server is broken".

An unregistered channel name becomes a `ZLinkConfigurationException`, and this route turns that
into 400.

```bash
curl -X POST 'http://127.0.0.1:5481/admin/channels/no-such-channel/weight?value=50'
# 400 {"error":"RouteMesh channel 'no-such-channel' is not registered."}
```

### 7. Spot — addressing by id

Every call so far picked its target by name: give a channel name and the framework picks one of
the nodes owning it; give a routing id and that node answers. Spot is different. **Give one id,
and the room with that id goes to whichever node it is on right now.**

```bash
curl -X POST http://127.0.0.1:5480/rooms   -H 'Content-Type: application/json' -d '{"title":"lobby"}'
# "9d36f685-7057-42ad-a341-f7ef080df496"

curl -X POST http://127.0.0.1:5480/rooms/9d36f685-7057-42ad-a341-f7ef080df496/chat   -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'
# 202

curl http://127.0.0.1:5480/rooms/9d36f685-7057-42ad-a341-f7ef080df496
# {"title":"lobby","chat":["p1: hello"]}
```

The id is generated by the framework. The first call is one-way and does not wait for a response;
the second gets back the room's own answer. The room held state between the two calls.

Things worth knowing on the Node side:

- **The moment you register one Spot, you need both a Location Store and a Relocation Store.**
  Registration itself is the condition, so even with relocation turned off, a Relocation Store is
  required.
- **The calling client differs from channels.** Channel calls use `ZLINK_ROUTE_CLIENT`, Spot
  calls use `ZLINK_SPOT_OUTBOUND`, and creating a room uses `ZLINK_SPOT_MANAGER` — matching
  .NET's `IZLinkSpotClient`/`IZLinkSpotManager` respectively.
- **A Node user Spot always names the actor type it admits.** This room admits no actors, so it
  uses the default type and rejects every join. .NET's `IZLinkSpot` has no such type argument.

### 8. Actor — a player addressed by id

Where a room is a place several people share, an Actor is one entity. Its id is **chosen by the
caller**, and creating it again with the same id returns the existing one.

```bash
curl -X POST http://127.0.0.1:5480/players/p7   -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
# "created"   — repeating the same call returns "existing"

curl http://127.0.0.1:5480/players/p7
# {"playerId":"p7","nickname":"anonymous"}

curl -X POST http://127.0.0.1:5480/players/p7/nickname   -H 'Content-Type: application/json' -d '{"nickname":"veteran"}'
# 202

curl http://127.0.0.1:5480/players/p7
# {"playerId":"p7","nickname":"veteran"}
```

Things worth knowing on the Node side:

- **Actors are not built with a constructor.** The Framework builds them through a factory, so
  `Player` is registered as `Scope.TRANSIENT` and takes its dependencies from `PlayerFactory`.
- **You must register one Entry Spot.** It is where a newly created player first enters, one per
  Object Server.
- **Handlers receive both the Spot and the Actor.** A message sent to an actor id runs inside
  whichever Spot that Actor currently belongs to.

### 9. Location — looking up placement

Spot and Actor were both addressed only by id, and the Framework found where they were. This call
reads that record directly.

```bash
curl http://127.0.0.1:5480/locations/rooms/832cf03d-0f75-4c1d-867f-e9b6f84fa247
# {"spotId":"832cf03d-0f75-4c1d-867f-e9b6f84fa247","node":"game-server-1"}

curl http://127.0.0.1:5480/locations/players/p7
# {"actorId":"p7","node":"game-server-1"}

curl -i http://127.0.0.1:5480/locations/players/ghost
# 404
```

The lookup only reads the Location Store and sends nothing to the target. Only a target that can
receive messages right now answers, so a value being created or in the middle of relocating comes
back empty.

### 10. STREAM and the Session-Actor link

An external client attaches. It references the connector only, not the framework.

```bash
cd StreamClient
npm install
npm run build
npm start
```

```
connected: true
round trip: 6ms          # STREAM request/reply
bound player: p1         # binds the connection to a player
pushed: speedy           # the player pushes over that connection
```

`pushed` is the key line. The client only sent a nickname change, and instead of a response, it
received **a notification the player itself pushed**.

Things worth knowing on the Node side:

- **Stream transport is WebSocket.** Both endpoints are `ws://`. The .NET/C++/Java/Kotlin
  connectors use TCP and write `tcp://`.
- **The connector ships as ESM only.** That is why `StreamClient` is a separate project, and
  relative imports carry a `.js` extension.
- **Session handlers are not auto-scanned.** `GameSessionFactory.create` registers them with
  `context.handlers.addHandler(...)`, and `@ZLinkPacket` decides the packet name.
- **`client.reply` only answers a Request.** To push to a client with no pending request, use
  `context.boundSession.send(...)`.

## Actual output

```
$ node --version
v22.23.2
$ npm --version
10.9.8
$ npm install

added 44 packages, and audited 45 packages in 2m
$ npm run build

> zlink-tutorial@0.0.0 build
> tsc -p tsconfig.json
$ node dist/Server/main.js
[Nest] ... LOG [NestFactory] Starting Nest application...
[Nest] ... LOG [InstanceLoader] ServerModule dependencies initialized
[Nest] ... LOG [InstanceLoader] DiscoveryModule dependencies initialized
[Nest] ... LOG [InstanceLoader] ZLinkModule dependencies initialized
server listening on tcp://0.0.0.0:7701 (mesh "game", routing id "game-server-1")
server admin listening on http://127.0.0.1:5481
$ node dist/Client/main.js
[Nest] ... LOG [NestFactory] Starting Nest application...
[Nest] ... LOG [InstanceLoader] ClientModule dependencies initialized
[Nest] ... LOG [InstanceLoader] DiscoveryModule dependencies initialized
[Nest] ... LOG [InstanceLoader] ZLinkModule dependencies initialized
client listening on http://127.0.0.1:5480
```

```
$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/profile
{"playerId":"p1","nickname":"rookie","level":1}
HTTP_STATUS:200

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/logins

HTTP_STATUS:202

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/tickets
"ticket-p1"
HTTP_STATUS:200

$ curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"message":"scheduled maintenance"}' \
    -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/notices

HTTP_STATUS:202

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/game-server-1/status
{"meshName":"game","channelName":"(none)","calledBy":"game-1552add6-0517-4bf3-bd6b-a18306bc7ae3","uptime":"42s","processId":3632973}
HTTP_STATUS:200

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/no-such-node/status
{"error":"not_found","message":"MeshNode 'game' request failed with result 102 and errno 14."}
HTTP_STATUS:404
```

Below is the same run continuing on: weight dropped to 0, the same five calls sent again, then
weight restored to 100.

```
$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' \
    'http://127.0.0.1:5481/admin/channels/profile/weight?value=0'
{"channel":"profile","weight":0}
HTTP_STATUS:200

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/profile
{"error":"unavailable","message":"MeshNode 'game' request failed with result 102 and errno 0."}
HTTP_STATUS:503

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/logins
{"error":"unavailable","message":"One-way send route is not connected."}
HTTP_STATUS:503

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/tickets
"ticket-p1"
HTTP_STATUS:200

$ curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"message":"scheduled maintenance"}' \
    -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/notices

HTTP_STATUS:202

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/game-server-1/status
{"meshName":"game","channelName":"(none)","calledBy":"game-1552add6-0517-4bf3-bd6b-a18306bc7ae3","uptime":"44s","processId":3632973}
HTTP_STATUS:200

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' \
    'http://127.0.0.1:5481/admin/channels/profile/weight?value=100'
{"channel":"profile","weight":100}
HTTP_STATUS:200

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/profile
{"playerId":"p1","nickname":"rookie","level":1}
HTTP_STATUS:200

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/logins

HTTP_STATUS:202

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/players/p1/tickets
"ticket-p1"
HTTP_STATUS:200

$ curl -s -X POST -H 'Content-Type: application/json' \
    -d '{"message":"scheduled maintenance"}' \
    -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/notices

HTTP_STATUS:202

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/game-server-1/status
{"meshName":"game","channelName":"(none)","calledBy":"game-1552add6-0517-4bf3-bd6b-a18306bc7ae3","uptime":"46s","processId":3632973}
HTTP_STATUS:200

$ curl -s -w '\nHTTP_STATUS:%{http_code}\n' http://127.0.0.1:5480/ops/nodes/no-such-node/status
{"error":"not_found","message":"MeshNode 'game' request failed with result 102 and errno 14."}
HTTP_STATUS:404

$ curl -s -X POST -w '\nHTTP_STATUS:%{http_code}\n' \
    'http://127.0.0.1:5481/admin/channels/no-such-channel/weight?value=50'
{"error":"RouteMesh channel 'no-such-channel' is not registered."}
HTTP_STATUS:400
```

While weight was 0, call 1 ends with `errno 0` and call 2 with
`One-way send route is not connected.` — different from the `errno 14` above for calling a node
with no name. Calls 3, 4 and 5 kept getting 200/202/200 through the same window.

**Both failures while weight is 0 share the same error kind** — both are `Unavailable`, hence
503. It means no member was left when candidates were picked, and the send path and connection
are still intact, so it is not `NotFound`. Framework 0.16.0 unified request and one-way onto this
single kind ([#498]); before that, request used `ProtocolError`, hence 400.

Only the call to a node with no name gets `NotFound`, hence 404 — it addresses by routing id
without picking a candidate, and no node knows that id. Not lumping these into a single 500 is
why `Client/zlink-error-response.ts` exists.

[#498]: https://github.com/zlink-systems/zlink/issues/498

`calledBy`, `uptime` and `processId` differ on every run. The values above came from one run.

## How the documentation reads this

The documentation never copies code by hand; it reads a span out of these files. The span is
marked by `--8<--` markers in the source. Marker names match the .NET tutorial.

| Marker | Location |
|---|---|
| `channel-contracts` | `Shared/contracts.ts` |
| `clientserver-contracts` | `Shared/contracts.ts` |
| `fanout-contracts` | `Shared/contracts.ts` |
| `node-direct-contracts` | `Shared/contracts.ts` |
| `channel-request-handler` | `Server/Channel/get-player-profile-handler.ts` |
| `channel-send-handler` | `Server/Channel/record-login-handler.ts` |
| `clientserver-handler` | `Server/Channel/issue-session-ticket-handler.ts` |
| `fanout-handler` | `Server/Channel/maintenance-notice-subscriber.ts` |
| `node-direct-handler` | `Server/Ops/node-status-handler.ts` |
| `filter-implementation` | `Server/Dispatch/call-log-filter.ts` |
| `filter-register` | `Server/main.ts` |
| `weight-runtime` | `Server/main.ts` |
| `mesh-register` | `Server/main.ts` |
| `channel-register` | `Server/main.ts` |
| `node-direct-register` | `Server/main.ts` |
| `clientserver-register` | `Server/main.ts` |
| `fanout-subscribe` | `Server/main.ts` |
| `channel-client-register` | `Client/main.ts` |
| `clientserver-client-register` | `Client/main.ts` |
| `fanout-publish-register` | `Client/main.ts` |
| `channel-request-call` | `Client/main.ts` |
| `channel-send-call` | `Client/main.ts` |
| `node-direct-call` | `Client/main.ts` |
| `clientserver-call` | `Client/main.ts` |
| `fanout-call` | `Client/main.ts` |
| `spot-contracts` | `Shared/contracts.ts` |
| `spot-class` / `spot-handlers` | `Server/Spots/game-room.ts` |
| `location-store` / `relocation-store` | `Server/main.ts` |
| `object-server` / `spot-register` | `Server/main.ts` |
| `location-store-client` / `spot-client-register` | `Client/main.ts` |
| `spot-create-call` / `spot-message-call` | `Client/main.ts` |
| `spot-send-call` / `spot-request-call` | `Client/main.ts`, split inside `spot-message-call` |
| `location-find` | `Client/main.ts` |
| `actor-contracts` | `Shared/contracts.ts` |
| `actor-class` / `actor-factory` / `actor-handlers` | `Server/Actors/player.ts` |
| `actor-send-handler` / `actor-request-handler` / `actor-push` | `Server/Actors/player.ts`, split inside `actor-handlers` |
| `entry-spot` | `Server/Spots/lobby-spot.ts` |
| `actor-register` | `Server/main.ts` |
| `actor-create-call` / `actor-send-call` / `actor-request-call` | `Client/main.ts` |
| `stream-contracts` / `session-actor-contracts` | `Shared/contracts.ts` |
| `session-class` / `session-actor-relay` | `Server/Sessions/game-session.ts` |
| `session-handler` | `Server/Sessions/ping-handler.ts` |
| `session-actor-bind` | `Server/Sessions/authenticate-handler.ts` |
| `stream-register` | `Server/main.ts` |
| `stream-client` / `session-actor-client` | `StreamClient/main.ts` |
| `error-mapping` | `Client/zlink-error-response.ts` |

## Where this differs from the .NET tutorial on the surface

Same idea, different names and locations.

| Topic | .NET | Node |
|---|---|---|
| Outgoing message contract | `record`. Anything works | **Must be a `class`.** The packet name comes from `payload.constructor.name`, which rejects `'Object'`, so an object literal cannot be used. A receive-only response type can stay an `interface` |
| Handler registration | `AddRequestHandler<THandler, TReq, TRes>()`. The name comes from the type | `addRequestHandler(packetName, HandlerType)`. The packet name is given as a string |
| Handler construction | The DI container finds it in the assembly | Registered directly as a NestJS `provider`. Filters are the same |
| Filter registration | `options.UseFilter<CallLogFilter>()` | `builder.options({ filters: [CallLogFilter] })`. Array order is execution order |
| Pinning a routing id | `SetRoutingId(RoutingId.From("game-server-1"))` | `routingId('game-server-1')`. Node's `RoutingId` is a `string` alias |
| Wildcard bind | `Listen("tcp://0.0.0.0:7201")` alone works | The advertise host must be given too. Without it, `ZLinkConfigurationException` blocks startup |
| Direct-node handler | `mesh.AddRouteRequestHandler<...>()` | `mesh.addRequestHandler(packetName, Type)` — the same name as the channel side; distinguished by not going through `mesh.channel(...)` |
| Fanout subscription handler | `AddHandler<TSub, TMsg>()` | `addPublishHandler(packetName, Type)` |
| ClientServer call | `IZLinkRouteClient.RequestToChannel(...)` handles both mesh channel and ClientServer | **The client differs.** Mesh channel and direct-node calls use `ZLINK_ROUTE_CLIENT`; ClientServer uses `ZLINK_CHANNEL_CLIENT`. `ZLinkRouteClient.requestToChannel` only finds mesh channels |
| Call terminator | `.Async<T>(ct)` | `.submit<T>(signal?)`. There is no synchronous terminator |
| HTTP surface | ASP.NET Core `app.MapGet(...)` | `node:http`. Nowhere in this repository's Node code uses the NestJS HTTP module (same call as quickstart) |
| Accessing runtime weight | `IZLinkRouteMeshRuntimeOptions` is injected as a handler argument | Pulled from the `ZLINK_ROUTE_MESH_RUNTIME_OPTIONS` token via `app.get<ZLinkRouteMeshRuntimeOptions>(..., { strict: false })`. The provider is only registered for a registration that has called `addRouteMesh` at least once |
| Assigning runtime weight | `mesh.Channel(channel).Weight = value` | `mesh.channel(channel).weight = value`. Only the name changes to camelCase. `channel(...)` returns a getter/setter pair rather than a copied value object, so the assignment itself is the runtime change |
| Passing the weight argument | ASP.NET Core binds `int value` from the query string | Reads `url.searchParams.get('value')` directly with `Number(...)`. Outside the integer range 0..10000 it is a `ZLinkConfigurationException`, which this tutorial turns into 400 |
| Process lifetime | `app.RunAsync()` | `NestFactory.createApplicationContext(...)`. Starts only the DI context, with no HTTP listener |
| Framework exception → HTTP status | One `IApplicationBuilder.Use(...)` middleware registered ahead of the endpoint | **A NestJS `ExceptionFilter` cannot be used.** A filter only runs inside Nest's request pipeline. This process boots with `createApplicationContext`, so it has no HTTP adapter, and `INestApplicationContext` has no `useGlobalFilters` either. Instead, `withZLinkErrorResponse(...)` wraps the entire route dispatch once. The mapping table and body shape match .NET |

## Where this differs from the .NET tutorial in meaning

| Topic | .NET | Node |
|---|---|---|
| How a Fanout subscriber finds the publisher | The Location Store provides it. Using a manual `Connect` alongside it is rejected | Given the endpoint directly with `enableSubscriber('tcp://127.0.0.1:7712')`. This is why it runs without a Store at all. Calling `enableSubscriber()` with no argument is the Store-backed form, and mixing the two on one channel blocks startup |
| `PeerConnections.Connect(RoutingId, endpoint)` | Without it, a direct-node call is rejected as not knowing its target | **Not rejected.** Confirmed by swapping this tutorial's compiled output to `connect('tcp://127.0.0.1:7701')` without a routing id and running it — both the channel call and the direct-node call still got 200. On Node, giving a routing id is closer to recording "this node lives at this endpoint" |
| Location Store / Relocation Store registration | Present on both Server and Client (because of Spot/Actor) | Same. Once the Spot stage arrives, the Server registers both and the Client registers the Location Store |
| `Objects().Server()` / `Objects().Client()` | Present on both Server and Client | Same. Not calling it means "no role", so this call was absent before the Spot stage |
| Fanout publisher identity | Optional | **Required once a Location Store is registered.** The Store keeps one row per publisher, so either `setRoutingIdPrefix(...)` or `routingId(...)` is needed. Without one, startup is blocked by `channel 'broadcast' publisher must select a fixed routing id or an automatic routing id prefix.` This line was absent at the stage before the Store existed |
| Server's HTTP | Opens the one `weight-runtime` endpoint on 5081 | Opens the same one endpoint on 5481. Only the port differs, because the client uses 5480 |
| Computing `uptime` | `DateTime.Now - Process.GetCurrentProcess().StartTime` | `process.uptime()`. Measures the same thing, but Node already gives it in seconds |

## Where the contract was checked

Written against the **public contract and working code**, not the guide documentation (checked
inside the repository — that source is not in this zip). The places read were
`framework/languages/node/packages/framework/src/contracts/`,
`framework/languages/node/packages/nestjs/src/`,
`framework/languages/node/e2e/`. Where the comparison disagreed with documentation:

| Topic | What the documentation says |
|---|---|
| `ZLinkRouteClient` carries `sendToSpot`/`requestToSpot` | The Node interface spec, chapter 02 §4, puts both on `ZLinkRouteClient` with a `spotId: SpotId` argument. The public contract `contracts/Channels/RouteCalls.ts`'s `ZLinkRouteClient` has neither (they are on `ZLinkSpotClient`); the implementation `DefaultZLinkRouteClient` has them, but with a `SpotHandle` argument |
| The NestJS builder's way to name a Fanout subscription topic | Chapter 02 §1's `ZLinkFanoutChannelBuilder` has `subscribe(topic)`, `connect(endpoint)` and `subscriberConnections()`. `@zlink-systems/nestjs`'s `ZLinkNestFanoutChannelBuilder` has none of the three, only `enableSubscriber(endpoint?)`. Registering no topic at all subscribes to everything under an empty prefix |
| `@zlink-systems/zlink@1.2.0`'s prebuild coverage (resolved by framework 0.18.1's `1.2.1`) | The package's `files` field was written to ship `prebuilds/win32-*/*.dll` and `prebuilds/darwin-*/*.dylib`. 1.2.0's published tarball carried only `prebuilds/linux-x64/`, so Windows and macOS fell back to a source build (#656). 1.2.1 adds `prebuilds/win32-x64/`, and `@zlink-systems/framework` pins that version starting at 0.18.1 — `darwin-*` is still missing |
