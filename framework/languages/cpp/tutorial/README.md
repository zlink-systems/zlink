[English](./README.md) | [한국어](./README.ko.md)

# C++ Tutorial

The program the feature guides read their code from. Follow the chapters one by one and this
program grows in the same order. It is the `.NET Tutorial` ported to C++: the four kinds of
Channel messaging (RouteMesh request and one-way, node direct call, ClientServer, Fanout),
handler filters, runtime weight changes, Spot, Actor, Location, STREAM and the HTTP client.

This directory is `tutorial/` in the `zlink-cpp-examples` repository. The procedure below uses
the platform-specific framework prebuilt published on GitHub Releases.

| | Purpose |
|---|---|
| quickstart (repository `framework/languages/cpp/quickstart/`) | Install through the first reply. Adds no features |
| **tutorial** (here) | Adds features one at a time. The feature guides read this code |
| samples (`samples/` in the `zlink-cpp-examples` repository) | Applications with a complete business flow |

## Contents

- [Prerequisites](#prerequisites)
- [Download and install](#download-and-install)
- [Build](#build)
- [Run](#run)
- [Verify](#verify)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [Step by step](#step-by-step)
- [How the documentation reads this code](#how-the-documentation-reads-this-code)
- [Differences from the .NET tutorial](#differences-from-the-net-tutorial)

The command blocks of `Build`, `Run` and `Verify` are marked `title="linux"` (bash) and
`title="windows"` (PowerShell). Each block runs as is from `tutorial/` after cloning the
`zlink-cpp-examples` repository, and the release CI runs the same blocks verbatim.

## Prerequisites

Bash blocks run on Linux, macOS, and WSL; PowerShell blocks run on Windows PowerShell 7. `cmd` is not supported.

| Tool | Windows | Linux / WSL |
|---|---|---|
| C++20 compiler | Visual Studio 2022 17.4 or later or Visual Studio 2026 with the **Desktop development with C++** workload | GCC 13 or later (verified with 13.3) |
| CMake | 3.24 or later (the 3.31 Visual Studio installs was used) | 3.24 or later (3.28 was used) |
| Ninja | not needed | recommended; Makefiles are used when it is absent |
| Docker Desktop | runs one Redis container. Must be installed and running | same (WSL integration, or Docker Engine on Linux) |

Nothing else is needed: no zlink repository, package manager, Node.js, or distribution Boost.
The bootstrap downloads and extracts the published prefix; later installs reuse it locally.

## Download and install

Clone the [`zlink-cpp-examples`](https://github.com/zlink-systems/zlink-cpp-examples) repository.
Every command below runs inside its `tutorial/` directory.

One script, `bootstrap.cmake`, does the install -- it is the first line of the [Build](#build)
block. It downloads this platform's framework prebuilt from `framework-cpp/v<version>`, extracts
its consumer prefix into `.zlink/install/`, and configures this project into `build/`. The one
prefix includes Core, the C++ binding, the framework libraries, and `nlohmann_json`, so no package
manager runs on the consumer machine. From the second run on it reuses the downloaded archive and
extracted prefix. To start over, delete `.zlink/` and `build/`.

## Build

**Linux · macOS · WSL — bash**

```bash title="linux"
cmake -P bootstrap.cmake
cmake --build build --parallel
```

**Windows — PowerShell 7**

```powershell title="windows"
cmake -P bootstrap.cmake
cmake --build build --config Release --parallel
```

Four executables come out -- under `build\Release\` on Windows, `build/` on Linux. On Windows
the Core `zlink.dll` and the third-party DLLs are copied next to the executables (Windows has
no RPATH; the loader only looks beside the image).

## Run

Redis must be at `127.0.0.1:6379`; the Spot, Actor and Location steps use it as the Location
Store. The block below starts Redis with Docker, then the Server and the Client, and confirms
with the first request that the two processes are connected over the mesh. Handler and filter
logs go to **stderr**.

**Linux · macOS · WSL — bash**

```bash title="linux"
docker run -d --rm --name zlink-tutorial-redis -p 127.0.0.1:6379:6379 redis:7-alpine && until docker exec zlink-tutorial-redis redis-cli ping 2>/dev/null | grep -q PONG; do sleep 0.2; done
./build/tutorial_server > server.log 2>&1 &
echo $! > server.pid
./build/tutorial_client > client.log 2>&1 &
echo $! > client.pid
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5180/players/p1/profile > /dev/null && break; sleep 1; done
```

**Windows — PowerShell 7**

```powershell title="windows"
docker run -d --rm --name zlink-tutorial-redis -p 127.0.0.1:6379:6379 redis:7-alpine | Out-Null; if ($LASTEXITCODE -eq 0) { while (-not ((docker exec zlink-tutorial-redis redis-cli ping 2>$null) -match 'PONG')) { Start-Sleep -Milliseconds 200 } }
$server = Start-Process -NoNewWindow .\build\Release\tutorial_server.exe -RedirectStandardOutput server.out -RedirectStandardError server.log -PassThru
$server.Id | Set-Content server.pid
$client = Start-Process -NoNewWindow .\build\Release\tutorial_client.exe -RedirectStandardOutput client.out -RedirectStandardError client.log -PassThru
$client.Id | Set-Content client.pid
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5180/players/p1/profile; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
if ($LASTEXITCODE -ne 0) { throw 'tutorial-http did not come up' }
```

In PowerShell `curl` is an alias of `Invoke-WebRequest`, so use `curl.exe` and escape the double
quotes of a JSON body as `\"`. The request that opens a room, for example:

```powershell
curl.exe -X POST http://127.0.0.1:5180/rooms -H 'Content-Type: application/json' -d '{\"title\":\"lobby\"}'
```

```bash
curl -X POST http://127.0.0.1:5180/rooms -H 'Content-Type: application/json' -d '{"title":"lobby"}'
```

The external client of the STREAM step is the third executable. The HTTP client step is the fourth
executable. Run both while the Server and Client are up; each completes its own check and exits.

Cleanup stops the two processes and the Redis container.

```powershell
Get-Content client.pid, server.pid | ForEach-Object {
  if ($_ -match '^\d+$') { taskkill /PID $_ /T /F 2>$null | Out-Null }
}
docker stop zlink-tutorial-redis
```

```bash
for pid in "$(cat client.pid)" "$(cat server.pid)"; do
  pkill -TERM -P "$pid" 2>/dev/null || true
  kill "$pid" 2>/dev/null || true
done
docker stop zlink-tutorial-redis
```

The ports differ from the .NET tutorial so both can run on one machine.

| Use | Port |
|---|---|
| Client HTTP | 5180 |
| Server HTTP (operations endpoint) | 5181 |
| Server mesh | 7401 |
| Client mesh | 7402 |
| ClientServer channel | 7411 |
| Fanout publisher | 7412 |
| Server stream node | 7421 |

## Verify

Examples smoke runs this block exactly as written.

| Step | Evidence of success |
|---|---|
| `cmake -P bootstrap.cmake` | last line `-- bootstrap done. Next: cmake --build ...`; `.zlink/install/lib/cmake/zlink_framework/zlink_frameworkConfig.cmake` exists |
| Build | the four executables `tutorial_server`, `tutorial_client`, `tutorial_stream_client`, `tutorial_http_client` exist |
| First request | `curl http://127.0.0.1:5180/players/p1/profile` prints `{"level":1,"nickname":"rookie","playerId":"p1"}` |
| Spot (Redis) | the request that opens a room prints a room id string (`"9e78fd70-..."`) |
| Instance Spot | two requests for the same queue id return `waiting` 1, then 2 |
| STREAM | `tutorial_stream_client` prints `pushed: speedy, actor: p1`, then the per-handle pushes for p1 and p2, and exits with 0 |

The block below checks this against the processes the [Run](#run) block started: the first
request's answer and the STREAM client's exit code.

**Linux · macOS · WSL — bash**

```bash title="linux"
set -e
curl -sf http://127.0.0.1:5180/players/p1/profile | grep -q '"playerId":"p1"'
echo "tutorial-http=ok"
./build/tutorial_stream_client
echo "tutorial-stream=ok"
```

**Windows — PowerShell 7**

```powershell title="windows"
if ((curl.exe -s http://127.0.0.1:5180/players/p1/profile) -notmatch '"playerId":"p1"') { throw 'tutorial-http failed' }
Write-Output 'tutorial-http=ok'
& .\build\Release\tutorial_stream_client.exe
if ($LASTEXITCODE -ne 0) { throw 'tutorial-stream failed' }
Write-Output 'tutorial-stream=ok'
```

[Step by step](#step-by-step) lists the request and expected output of all eleven steps.

## Stop

Stop the processes started by the Run section.

**Linux · macOS · WSL — bash**

```bash title="linux"
for pid in "$(cat client.pid)" "$(cat server.pid)"; do
  pkill -TERM -P "$pid" 2>/dev/null || true
  kill "$pid" 2>/dev/null || true
done
docker rm -f zlink-tutorial-redis 2>/dev/null || true
```

**Windows — PowerShell 7**

```powershell title="windows"
Get-Content client.pid, server.pid | ForEach-Object {
  if ($_ -match '^\d+$') { taskkill /PID $_ /T /F 2>$null | Out-Null }
}
Get-Job | Stop-Job -ErrorAction SilentlyContinue
docker rm -f zlink-tutorial-redis 2>$null | Out-Null
```

## Running from an IDE

`bootstrap.cmake` records the exact arguments it configured this folder with as the preset `zlink`
in `CMakeUserPresets.json`. Visual Studio, Rider and CLion read that preset when they open the
folder, so after one bootstrap the IDE continues in the same `build/`. The bootstrap rewrites the
file every time; it is not edited by hand.

1. Run `cmake -P bootstrap.cmake` from the [Build](#build) section once in a terminal (an IDE cannot
   run a `-P` script).
2. **Visual Studio 2022 or 2026**: File › Open › Folder on this directory. Pick the preset `zlink`
   in the CMake settings; configuration completes and the startup item list shows `tutorial_server` and `tutorial_client`. Start the
   server first, then the client.
3. **Rider or CLion**: open this directory's `CMakeLists.txt` as the project. Enable the preset
   `zlink` under Settings › Build, Execution, Deployment › CMake; configuration completes and run
   configurations for `tutorial_server` and `tutorial_client` appear.
4. Stop with the IDE's Stop button; the IDE ends the process tree, so the [Stop](#stop) section's
   commands are not needed.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `bootstrap: framework prebuilt is unavailable for this host platform: https://github.com/...` | GitHub Releases is unreachable, or the release has no archive for this platform. Check proxy, firewall, release version, and platform support, then rerun |
| `CMake Error ... No CMAKE_CXX_COMPILER could be found` / `Visual Studio 17 2022 could not find any instance` | No compiler. Install the **Desktop development with C++** workload on Windows, `g++` on Linux |
| `error LNK2038: mismatch detected for 'RuntimeLibrary'` | The extracted prefix or build tree does not match the selected toolchain. Delete `.zlink/` and `build`, then bootstrap again |
| On Windows an executable exits at once with no output (exit code `-1073741515`, `STATUS_DLL_NOT_FOUND`) | `zlink.dll` is not beside the executable. Rerun `cmake --build build --config Release`; the post-build step copies it into `build\Release\` |
| `docker: error during connect` / `Cannot connect to the Docker daemon` | Docker Desktop is not running. Start it and repeat `docker run ...` |
| `docker: Error response from daemon: ... port is already allocated` / `Bind for 127.0.0.1:6379 failed` | Another Redis owns 6379. Clear this tutorial's keys as shown below before reusing it, or start with a fresh Redis on 6379 |
| Server log reports a `Location Store` connection failure | No Redis. The Channel steps still work; the Spot, Actor and Location steps fail |
| Server fails to start after a forced stop | The previous owner lease can remain valid for up to 15 seconds ([default owner lease TTL](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md#L670-L674)). Wait for it to expire, then start the Server again; a failed start does not retry. To restart immediately, stop earlier tutorial processes and clear this tutorial's keys with the command below |
| `bind: Address already in use` / `Only one usage of each socket address` | Another process holds a port from the table above. Check for a `tutorial_server` or `tutorial_client` left from an earlier run |
| `curl: (7) Failed to connect to 127.0.0.1 port 5180` | The Client is not up yet, or died. Read the Client's stderr |

When reusing Redis, stop any earlier instance of this tutorial before clearing its Location and
Relocation Store records. Run the command for your shell before the [Run](#run) block, then omit
only its `docker run` line. These commands delete only keys under this tutorial's
`zlink-tutorial-cpp:` prefix; they require `redis-cli` connected to the Redis on 6379.

```bash title="linux"
redis-cli --scan --pattern 'zlink-tutorial-cpp:*' | while IFS= read -r key; do redis-cli DEL "$key" >/dev/null; done
```

```powershell title="windows"
redis-cli --scan --pattern 'zlink-tutorial-cpp:*' | ForEach-Object { redis-cli DEL $_ | Out-Null }
```

## Project layout

| Project | Role |
|---|---|
| `Shared` | Message contracts both sides share |
| `Server` | Runs the channel handlers and the node direct-call handler, and installs the filter. Opens HTTP for one operations endpoint |
| `Client` | Accepts HTTP and calls over the mesh |
| `StreamClient` | A client outside the mesh. Links the connector only, never the framework |
| `HttpClient` | A client outside the mesh. Links only the HTTP client package, never the framework |
| `bootstrap.cmake` | Installs the framework from the published archives and configures this project |

`CMakeLists.txt` builds the four executables from `find_package(zlink_framework CONFIG REQUIRED)` and
`find_package(zlink_http_client_cpp CONFIG REQUIRED)`. To reuse it in your own project, pass
`.zlink/install` in `CMAKE_PREFIX_PATH`.

## Step by step

Each feature can be read on its own; a step works without the ones before it. The outputs
below were all captured from real runs. The Korean README explains each step in depth; this
section keeps the requests and what they show.

### 1. Channel messaging -- RouteMesh

The caller does not pick a node. It names a channel, and a node serving that channel answers.

```console
$ curl http://127.0.0.1:5180/players/p1/profile
{"level":1,"nickname":"rookie","playerId":"p1"}

$ curl -i -X POST http://127.0.0.1:5180/players/p1/logins
HTTP/1.1 202 Accepted
Content-Length: 0
```

The second call is one-way; the Server's stderr shows the filter around the handler:

```
info class call_log_filter_t - dispatch start: RecordLogin
info class record_login_handler_t - login recorded: p1
info class call_log_filter_t - dispatch done: RecordLogin in 2ms
```

JSON keys are alphabetical: nlohmann JSON keeps objects as sorted maps.

### 2. Channel messaging -- node direct call

No channel involved. The receiver registers with `mesh.add_route_request_handler`; the caller
names the node's routing id. Operations commands only.

```console
$ curl http://127.0.0.1:5180/ops/nodes/game-server-1/status
{"calledBy":"game-client-1","channelName":"","meshName":"game","uptime":"10s"}

$ curl -i http://127.0.0.1:5180/ops/nodes/no-such-node/status
HTTP/1.1 404 Not Found
{"correlationId":"http-6","error":"not_found","message":"MeshNode request target was not found"}
```

An empty `channelName` is the point. The receiver must fix its id with `set_routing_id`, and the
caller must map that id to an endpoint with `peer_connections().connect(routing_id, endpoint)`.

### 3. Channel messaging -- ClientServer

Same calling code; the difference is **who receives**: the server the caller connected to.

```console
$ curl -X POST http://127.0.0.1:5180/players/p1/tickets
"ticket-p1"
```

RouteMesh calls use `route_client_t`, ClientServer calls use `channel_client_t`. Naming a
ClientServer channel through `route_client_t::request_to_channel` is refused with
`503 {"error":"unavailable","message":"RouteMesh channel 'ticketing' is not registered"}`.

### 4. Channel messaging -- Fanout

The publisher does not know the receivers; every subscribed node gets the event.

```console
$ curl -i -X POST http://127.0.0.1:5180/notices \
    -H 'Content-Type: application/json' -d '{"message":"scheduled maintenance"}'
HTTP/1.1 202 Accepted
Content-Length: 0
```

Server stderr:

```
info class call_log_filter_t - dispatch start: MaintenanceNotice
info class maintenance_notice_subscriber_t - maintenance notice: scheduled maintenance
info class call_log_filter_t - dispatch done: MaintenanceNotice in 1ms
```

Fanout handlers go into a named handler group that the channel picks up; there is no direct
`add_fanout_channel(...).add_handler<...>()`. Combining `connect(...)` with `enable_subscriber()`
is refused at startup: `fanout channel 'broadcast' cannot combine automatic discovery with manual
subscriber endpoints`.

### 5. Handler filter

The `dispatch start` / `dispatch done` pair around every call above is the filter. All four
paths -- channel request and one-way, ClientServer, Fanout, node direct call -- pass through it.
The category reads `class call_log_filter_t` because `logger_t<T>` defaults to `typeid(T).name()`
and MSVC prefixes that with `class `.

### 6. Runtime weight change

Everything so far was fixed at startup; weight is the one value changed while running. At 0 the
socket stays open and in-flight calls finish, but no other node picks this node for new calls.
The Server answers this endpoint itself.

```console
$ curl -u ops:tutorial-admin -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=0'
HTTP/1.1 200 OK
{"channel":"profile","weight":0}

$ curl -i http://127.0.0.1:5180/players/p1/profile
HTTP/1.1 503 Service Unavailable
{"correlationId":"http-7","error":"unavailable","message":"RouteMesh channel request was not submitted"}

$ curl -i -X POST http://127.0.0.1:5180/players/p1/logins
HTTP/1.1 404 Not Found
{"correlationId":"http-8","error":"not_found","message":"RouteMesh channel send target was not found"}

$ curl -u ops:tutorial-admin -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=100'
HTTP/1.1 200 OK
{"channel":"profile","weight":100}
```

ClientServer, fanout and node direct calls keep answering at weight 0: they are not RouteMesh
channel selections. Invalid input is refused with 400: a missing `value`
(`{"error":"value is required"}`), an unknown channel (`RouteMesh channel is not configured:
nope`) and `value=99999` (`channel weight must be in range 0..10000`).

### 7. Spot -- calling by id

One id, and the call goes to whichever node holds that room now.

```console
$ curl -X POST http://127.0.0.1:5180/rooms -H 'Content-Type: application/json' -d '{"title":"lobby"}'
"9e78fd70-edee-47b4-8433-d1539862917f"

$ curl -i -X POST http://127.0.0.1:5180/rooms/9e78fd70-edee-47b4-8433-d1539862917f/chat -H 'Content-Type: application/json' -d '{"playerId":"p1","text":"hello"}'
HTTP/1.1 202 Accepted

$ curl http://127.0.0.1:5180/rooms/9e78fd70-edee-47b4-8433-d1539862917f
{"chat":["p1: hello"],"title":"lobby"}
```

The Framework creates the id; the Location Store (Redis) records where it lives.

### 8. Instance Spot -- a queue the first message creates

There is no create call. The first message for an id makes the Framework create the queue,
then handles that same message.

```bash
curl -X POST http://127.0.0.1:5180/match-queues/ranked \
  -H 'Content-Type: application/json' -d '{"playerId":"p1"}'
# {"waiting":1}

curl -X POST http://127.0.0.1:5180/match-queues/ranked \
  -H 'Content-Type: application/json' -d '{"playerId":"p2"}'
# {"waiting":2}
```

The queue keeps what was added. Calling the same id again continues the count; use a new id
to start over.

### 9. Actor -- a player called by id

The **caller** picks the id; creating the same id again returns the existing actor.

```console
$ curl -X POST http://127.0.0.1:5180/players/p7 -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
"created"

$ curl -X POST http://127.0.0.1:5180/players/p7 -H 'Content-Type: application/json' -d '{"nickname":"rookie"}'
"existing"

$ curl http://127.0.0.1:5180/players/p7
{"nickname":"anonymous","playerId":"p7"}

$ curl -i -X POST http://127.0.0.1:5180/players/p7/nickname -H 'Content-Type: application/json' -d '{"nickname":"veteran"}'
HTTP/1.1 202 Accepted

$ curl http://127.0.0.1:5180/players/p7
{"nickname":"veteran","playerId":"p7"}
```

Actors are made by `player_factory_t`, not by a constructor; one Entry Spot must be registered.
`on_create_actor` admits the initial creation, and return to the Entry Spot commits without admission.

### 10. Location -- where is it

Reads the Location Store only; nothing is sent to the target.

```console
$ curl http://127.0.0.1:5180/locations/rooms/d50a66f2-fb52-4d4f-82a0-fdbeafd45511
{"node":"game-server-1","spotId":"d50a66f2-fb52-4d4f-82a0-fdbeafd45511"}

$ curl http://127.0.0.1:5180/locations/players/p7
{"actorId":"p7","node":"game-server-1"}

$ curl -i http://127.0.0.1:5180/locations/players/ghost
HTTP/1.1 404 Not Found
```

### 11. STREAM and the Session-Actor link

An external client attaches over TCP, linking the connector only. This is output from a complete
server and StreamClient run; the round-trip time varies.

```console
$ ./build/tutorial_stream_client
connected: true
round trip: 0ms
bound player: p1
actor bound: p1
pushed: speedy, actor: p1
actor bound: p2
bound player: p2
actor handle: p1
actor handle: p2
received actor id: p1
received actor id: p2
pushed: speedy-p1, actor: p1
pushed: speedy-p2, actor: p2
```

The first nickname change uses the connector directly while only p1 is bound. After p2 binds,
each handle addresses and receives for its own Actor; the connector-level receive callback also
shows each push's Actor ID. In C++ every packet arrives through one `on_packet`, `reply_packet`
answers requests only (pushes use `bound_session().send(...)`), and the connector uses immediate
dispatch so receive callbacks run when their pushes arrive.

### 12. HTTP client

`HttpClient` runs outside the mesh and links only the `zlink::http_client` package. The CLI awaits
the asynchronous `async<T>()`, `async_raw()`, `fetch<T>()`, and `download()` terminators. Run it
while the Server and Client are up:

**Linux · macOS · WSL — bash**

```bash title="linux"
./build/tutorial_http_client
```

**Windows — PowerShell 7**

```powershell title="windows"
& .\build\Release\tutorial_http_client.exe
```

The output below is from one run; the room id and download byte count vary per run.

```console
first request: p1 rookie
request shaping: status 200 weight 2
json body: player 200 room fbf674c7-9328-4624-a74a-7bccdab703f6 chat 202
response kinds: typed 200 raw application/json fetch speedy-p2
compressed response: 200 encoding-removed true
redirect: 200 p1
basic auth: without 401 with 200
download stream: chunks 1 bytes 74
upload stream: imported 3
error kinds: bad request internal_failure connection refused unavailable
```

This output is based on framework 0.19.0. The C++ HTTP host does not provide gzip or chunked
responses, so step 6 checks the plain response and step 9 receives one buffered chunk.

The HTTP surface includes Basic auth for the admin route, a legacy player-path redirect, and NDJSON
room export/import routes. The admin credential is hard-coded as `ops` / `tutorial-admin` because
the tutorial deliberately has no configuration file.

```console
$ curl -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=2'
HTTP/1.1 401 Unauthorized
WWW-Authenticate: Basic realm="tutorial-admin"

$ curl -u ops:tutorial-admin -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=2'
HTTP/1.1 200 OK
{"channel":"profile","weight":2}

$ curl -i http://127.0.0.1:5180/player/p1
HTTP/1.1 301 Moved Permanently
Location: /players/p1

$ curl http://127.0.0.1:5180/rooms/<room-id>/export
{"roomId":"<room-id>"}
{"message":"p2: hello"}

$ curl -X POST http://127.0.0.1:5180/rooms/<room-id>/import \
    -H 'Content-Type: application/x-ndjson' \
    --data-binary $'{"playerId":"p1","text":"one"}\n{"playerId":"p2","text":"two"}\n'
{"imported":2}
```

## How the documentation reads this code

The documentation does not copy code; it reads regions of these files, delimited by `--8<--`
markers in the source:

```
--8<-- "framework/languages/cpp/tutorial/Server/main.cpp:channel-register"
```

Renaming a marker makes the page that reads it emit an empty block, so rename the marker and
the page together. Marker names match the .NET tutorial.

| Marker | Location |
|---|---|
| `channel-contracts` | `Shared/contracts.hpp` |
| `channel-request-handler` | `Server/channel/get_player_profile_handler.hpp` |
| `channel-send-handler` | `Server/channel/record_login_handler.hpp` |
| `mesh-register` | `Server/main.cpp` |
| `channel-register` | `Server/main.cpp` |
| `channel-client-register` | `Client/main.cpp` |
| `channel-request-call` | `Client/main.cpp` |
| `channel-send-call` | `Client/main.cpp` |
| `node-direct-contracts` | `Shared/contracts.hpp` |
| `node-direct-handler` | `Server/ops/node_status_handler.hpp` |
| `node-direct-register` | `Server/main.cpp` |
| `node-direct-call` | `Client/main.cpp` |
| `filter-implementation` | `Server/dispatch/call_log_filter.hpp` |
| `filter-register` | `Server/main.cpp` |
| `clientserver-contracts` | `Shared/contracts.hpp` |
| `clientserver-handler` | `Server/channel/issue_session_ticket_handler.hpp` |
| `clientserver-register` | `Server/main.cpp` |
| `clientserver-client-register` | `Client/main.cpp` |
| `clientserver-call` | `Client/main.cpp` |
| `fanout-contracts` | `Shared/contracts.hpp` |
| `fanout-handler` | `Server/channel/maintenance_notice_subscriber.hpp` |
| `fanout-subscribe` | `Server/main.cpp` |
| `fanout-publish-register` | `Client/main.cpp` |
| `fanout-call` | `Client/main.cpp` |
| `weight-runtime` | `Server/ops/channel_weight_handler.hpp` |
| `spot-contracts` | `Shared/contracts.hpp` |
| `spot-class` · `spot-handlers` | `Server/spots/game_room.hpp` |
| `location-store` · `relocation-store` | `Server/main.cpp` |
| `object-server` · `spot-register` | `Server/main.cpp` |
| `location-store-client` · `spot-client-register` | `Client/main.cpp` |
| `spot-create-call` · `spot-message-call` · `spot-send-call` · `spot-request-call` | `Client/main.cpp` |
| `instance-spot-contracts` | `Shared/contracts.hpp` |
| `instance-spot-class` · `instance-spot-handler` | `Server/spots/match_queue.hpp` |
| `instance-spot-register` | `Server/main.cpp` |
| `instance-spot-call` | `Client/main.cpp` |
| `location-find` | `Client/main.cpp` |
| `actor-contracts` | `Shared/contracts.hpp` |
| `actor-class` · `actor-factory` | `Server/actors/player.hpp` |
| `entry-spot` · `actor-handlers` · `actor-push` · `actor-send-handler` · `actor-request-handler` | `Server/spots/lobby_spot.hpp` |
| `actor-register` | `Server/main.cpp` |
| `actor-create-call` · `actor-send-call` · `actor-request-call` | `Client/main.cpp` |
| `stream-contracts` · `session-actor-contracts` | `Shared/contracts.hpp` |
| `session-class` · `session-handler` · `session-actor-bind` · `session-actor-relay` | `Server/sessions/game_session.hpp` |
| `stream-register` | `Server/main.cpp` |
| `stream-client` · `session-actor-client` · `single-actor-send` · `actor-handle-events` · `actor-handle-send` · `actor-handle-per-handle-receive` · `actor-id-receive` · `actor-handle-send-call` · `actor-handle-receive` | `StreamClient/main.cpp` |
| `http-client-create` | `HttpClient/main.cpp` |
| `http-first-request` | `HttpClient/main.cpp` |
| `http-request-shaping` | `HttpClient/main.cpp` |
| `http-json-body` | `HttpClient/main.cpp` |
| `http-response-kinds` | `HttpClient/main.cpp` |
| `http-compressed-response` | `HttpClient/main.cpp` |
| `http-redirect` | `HttpClient/main.cpp` |
| `http-basic-auth` | `HttpClient/main.cpp` |
| `http-download-stream` | `HttpClient/main.cpp` |
| `http-upload-stream` | `HttpClient/main.cpp` |
| `http-error-kinds` | `HttpClient/main.cpp` |

## Differences from the .NET tutorial

Places where the meaning changed in the port; renamings alone (`AddRouteMesh` ->
`add_route_mesh`) are not listed.

| Topic | .NET | C++ |
|---|---|---|
| Location Store for fanout | a Redis store lets subscribers find the publisher | subscribers name the publisher endpoint; the mesh node needs `set_object_role(object_role_t::none)`, otherwise it becomes an Object Server and requires a Location Store |
| Fanout handler registration | `AddFanoutChannel(...).AddHandler<...>()` | through a handler group; the channel builder has no direct handler API |
| ClientServer caller | `IZLinkRouteClient` covers mesh and ClientServer channels | two types: `route_client_t` for the mesh, `channel_client_t` for ClientServer |
| Fanout publish topic | `Publish("broadcast", notice)` | `publish("broadcast", topic, notice)`; the topic is the event's packet name, the subscriber handler's default |
| Mesh advertise host | `Listen("tcp://0.0.0.0:7201")` is enough | a wildcard bind needs `set_advertise_host`; the fanout publisher binds `tcp://127.0.0.1:7412` for the same reason |
| `NodeStatus.ProcessId` | present | absent; standard C++ has no process id API |
| `NodeStatus.Uptime` origin | `Process.StartTime` | static initialization; handler objects are created per call |
| Empty `ChannelName` | `null`, shown as `"(none)"` | empty `std::optional`, shown as `""` via `value_or("")` |
| Message contracts | records serialize as declared | a `to_json`/`from_json` pair is required; members stay snake_case, wire keys camelCase |
| Calling a missing node | 500 | 404 `MeshNode request target was not found` |
