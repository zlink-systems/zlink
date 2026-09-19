[English](./README.md) | [한국어](./README.ko.md)

# C++ Tutorial

The program the feature guides read their code from. Follow the chapters one by one and this
program grows in the same order. It is the `.NET Tutorial` ported to C++: the four kinds of
Channel messaging (RouteMesh request and one-way, node direct call, ClientServer, Fanout),
handler filters, runtime weight changes, Spot, Actor, Location and STREAM.

This directory closes on itself. The procedure below uses only the Core, binding and framework
archives published on GitHub Releases plus vcpkg; the zlink repository is never cloned.

| | Purpose |
|---|---|
| quickstart (repository `framework/languages/cpp/quickstart/`) | Install through the first reply. Adds no features |
| **tutorial** (here) | Adds features one at a time. The feature guides read this code |
| samples (`zlink-samples-cpp.zip`) | Applications with a complete business flow |

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
`title="windows"` (PowerShell). Each block runs as is from the unpacked directory, and the
release CI runs the same blocks verbatim.

## Prerequisites

| Tool | Windows | Linux / WSL |
|---|---|---|
| C++20 compiler | Visual Studio 2022 17.4 or later with the **Desktop development with C++** workload (verified with MSVC 19.44) | GCC 13 or later (verified with 13.3) |
| CMake | 3.24 or later (the 3.31 Visual Studio installs was used) | 3.24 or later (3.28 was used) |
| Ninja | not needed | recommended; Makefiles are used when it is absent |
| vcpkg | the copy Visual Studio installs is found automatically; for a separate clone set `VCPKG_ROOT` | `git clone https://github.com/microsoft/vcpkg` and `./bootstrap-vcpkg.sh`; set `VCPKG_ROOT` unless it lives at `$HOME/vcpkg` |
| Docker Desktop | runs one Redis container. Must be installed and running | same (WSL integration, or Docker Engine on Linux) |

Nothing else is needed: no zlink repository, no Python, no Node.js, no distribution Boost.
vcpkg builds the third-party libraries (Boost, nlohmann_json, lz4, protobuf, OpenSSL,
opentelemetry-cpp, redis-plus-plus) from source, so **the first install takes about 20
minutes** and several GB of disk. Later installs finish in minutes from vcpkg's binary cache.

The third-party versions are pinned with a vcpkg `builtin-baseline`. A clone older than that
commit needs `git -C $VCPKG_ROOT pull`.

## Download and install

Download
[`zlink-tutorial-cpp.zip`](https://github.com/zlink-systems/zlink/releases/latest/download/zlink-tutorial-cpp.zip)
and unpack it. Every command below runs inside the unpacked `zlink-tutorial-cpp/`.

```bash title="linux"
cd zlink-tutorial-cpp
```

```powershell title="windows"
Set-Location zlink-tutorial-cpp
```

One script, `bootstrap.cmake`, does the install -- it is the first line of the [Build](#build)
block. It downloads three GitHub Release assets -- this platform's Core prebuilt
(`core/v1.2.0`), the C++ binding source (`cpp/v1.2.0`) and the framework source
(`framework-cpp/v0.18.0`) -- builds the binding and the framework into `.zlink/install/`, and
configures this project into `build/`. Only the framework version is written in the script; the
Core and binding versions and the third-party list come from the framework archive. From the
second run on it reuses what it downloaded and built.

Parallelism defaults to the logical core count; lower it as `cmake -DZLINK_JOBS=4 -P
bootstrap.cmake`. To start over, delete `.zlink/` and `build/`.

## Build

```bash title="linux"
export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
cmake -P bootstrap.cmake
cmake --build build --parallel
```

```powershell title="windows"
cmake -P bootstrap.cmake
cmake --build build --config Release --parallel
```

Three executables come out -- under `build\Release\` on Windows, `build/` on Linux. On Windows
the Core `zlink.dll` and the third-party DLLs are copied next to the executables (Windows has
no RPATH; the loader only looks beside the image).

## Run

Redis must be at `127.0.0.1:6379`; the Spot, Actor and Location steps use it as the Location
Store. The block below starts Redis with Docker, then the Server and the Client, and confirms
with the first request that the two processes are connected over the mesh. Handler and filter
logs go to **stderr**.

```bash title="linux"
docker run -d --rm --name zlink-tutorial-redis -p 127.0.0.1:6379:6379 redis:7-alpine
./build/tutorial_server > server.log 2>&1 &
./build/tutorial_client > client.log 2>&1 &
for i in $(seq 1 60); do curl -sf http://127.0.0.1:5180/players/p1/profile && break; sleep 1; done
```

```powershell title="windows"
docker run -d --rm --name zlink-tutorial-redis -p 127.0.0.1:6379:6379 redis:7-alpine
Start-Process -NoNewWindow .\build\Release\tutorial_server.exe -RedirectStandardOutput server.out -RedirectStandardError server.log
Start-Process -NoNewWindow .\build\Release\tutorial_client.exe -RedirectStandardOutput client.out -RedirectStandardError client.log
foreach ($i in 1..60) { $answer = curl.exe -s http://127.0.0.1:5180/players/p1/profile; if ($LASTEXITCODE -eq 0) { break }; Start-Sleep -Seconds 1 }
$answer
```

In PowerShell `curl` is an alias of `Invoke-WebRequest`, so use `curl.exe` and escape the double
quotes of a JSON body as `\"`. The request that opens a room, for example:

```powershell
curl.exe -X POST http://127.0.0.1:5180/rooms -H 'Content-Type: application/json' -d '{\"title\":\"lobby\"}'
```

```bash
curl -X POST http://127.0.0.1:5180/rooms -H 'Content-Type: application/json' -d '{"title":"lobby"}'
```

The external client of the STREAM step is the third executable. Run it while the Server is up;
it completes its own check and exits -- the [Verify](#verify) block runs it.

Cleanup stops the two processes and the Redis container.

```powershell
Stop-Process -Name tutorial_server,tutorial_client
docker stop zlink-tutorial-redis
```

```bash
pkill -f build/tutorial_server; pkill -f build/tutorial_client
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

| Step | Evidence of success |
|---|---|
| `cmake -P bootstrap.cmake` | last line `-- bootstrap done. Next: cmake --build ...`; `.zlink/install/lib/cmake/zlink_framework/zlink_frameworkConfig.cmake` exists |
| Build | the three executables `tutorial_server`, `tutorial_client`, `tutorial_stream_client` exist |
| First request | `curl http://127.0.0.1:5180/players/p1/profile` prints `{"level":1,"nickname":"rookie","playerId":"p1"}` |
| Spot (Redis) | the request that opens a room prints a room id string (`"9e78fd70-..."`) |
| STREAM | `tutorial_stream_client` prints the four lines `connected: true` ... `pushed: speedy` and exits with 0 |

The block below checks this against the processes the [Run](#run) block started: the first
request's answer and the STREAM client's exit code.

```bash title="linux"
set -e
curl -sf http://127.0.0.1:5180/players/p1/profile | grep -q '"playerId":"p1"'
echo "tutorial-http=ok"
./build/tutorial_stream_client
echo "tutorial-stream=ok"
```

```powershell title="windows"
if ((curl.exe -s http://127.0.0.1:5180/players/p1/profile) -notmatch '"playerId":"p1"') { throw 'tutorial-http failed' }
Write-Output 'tutorial-http=ok'
& .\build\Release\tutorial_stream_client.exe
if ($LASTEXITCODE -ne 0) { throw 'tutorial-stream failed' }
Write-Output 'tutorial-stream=ok'
```

[Step by step](#step-by-step) lists the request and expected output of all ten steps.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `bootstrap: vcpkg was not found. Set VCPKG_ROOT ...` | No vcpkg. On Windows enable the **vcpkg package manager** component in the Visual Studio Installer; on either platform point `VCPKG_ROOT` at a vcpkg clone |
| `error: this vcpkg instance requires a manifest with a specified baseline` | Only after hand-editing the manifest `bootstrap.cmake` writes (`.zlink/manifest/vcpkg.json`). Delete `.zlink/manifest` and rerun |
| `error: no version database entry for <port> at <version>`, or another baseline error | The vcpkg clone predates the baseline commit. `git -C $VCPKG_ROOT pull`, then rerun |
| `bootstrap: download failed: https://github.com/...` | GitHub Releases is unreachable; check proxy and firewall. The next run downloads it again from the start |
| `CMake Error ... No CMAKE_CXX_COMPILER could be found` / `Visual Studio 17 2022 could not find any instance` | No compiler. Install the **Desktop development with C++** workload on Windows, `g++` on Linux |
| `error LNK2038: mismatch detected for 'RuntimeLibrary'` | Leftovers of a `.zlink/` built with other options. Delete `.zlink/build`, `.zlink/cpp`, `.zlink/install` and `build`, then bootstrap again |
| On Windows an executable exits at once with no output (exit code `-1073741515`, `STATUS_DLL_NOT_FOUND`) | `zlink.dll` is not beside the executable. Rerun `cmake --build build --config Release`; the post-build step copies it into `build\Release\` |
| `docker: error during connect` / `Cannot connect to the Docker daemon` | Docker Desktop is not running. Start it and repeat `docker run ...` |
| `docker: Error response from daemon: ... port is already allocated` / `Bind for 127.0.0.1:6379 failed` | Another Redis owns 6379. That one can be used as is -- the tutorial only looks at `127.0.0.1:6379` |
| Server log reports a `Location Store` connection failure | No Redis. The Channel steps still work; the Spot, Actor and Location steps fail |
| `bind: Address already in use` / `Only one usage of each socket address` | Another process holds a port from the table above. Check for a `tutorial_server` or `tutorial_client` left from an earlier run |
| `curl: (7) Failed to connect to 127.0.0.1 port 5180` | The Client is not up yet, or died. Read the Client's stderr |

## Project layout

| Project | Role |
|---|---|
| `Shared` | Message contracts both sides share |
| `Server` | Runs the channel handlers and the node direct-call handler, and installs the filter. Opens HTTP for one operations endpoint |
| `Client` | Accepts HTTP and calls over the mesh |
| `StreamClient` | A client outside the mesh. Links the connector only, never the framework |
| `bootstrap.cmake` | Installs the framework from the published archives and configures this project |

`CMakeLists.txt` builds the three executables from one `find_package(zlink_framework CONFIG
REQUIRED)`. To reuse it in your own project, pass `.zlink/install` in `CMAKE_PREFIX_PATH`.

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
$ curl -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=0'
HTTP/1.1 200 OK
{"channel":"profile","weight":0}

$ curl -i http://127.0.0.1:5180/players/p1/profile
HTTP/1.1 503 Service Unavailable
{"correlationId":"http-7","error":"unavailable","message":"RouteMesh channel request was not submitted"}

$ curl -i -X POST http://127.0.0.1:5180/players/p1/logins
HTTP/1.1 404 Not Found
{"correlationId":"http-8","error":"not_found","message":"RouteMesh channel send target was not found"}

$ curl -i -X POST 'http://127.0.0.1:5181/admin/channels/profile/weight?value=100'
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

### 8. Actor -- a player called by id

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

Actors are made by `player_factory_t`, not by a constructor; one Entry Spot must be registered,
and in C++ its `on_actor_join` admission callback is mandatory.

### 9. Location -- where is it

Reads the Location Store only; nothing is sent to the target.

```console
$ curl http://127.0.0.1:5180/locations/rooms/d50a66f2-fb52-4d4f-82a0-fdbeafd45511
{"node":"game-server-1","spotId":"d50a66f2-fb52-4d4f-82a0-fdbeafd45511"}

$ curl http://127.0.0.1:5180/locations/players/p7
{"actorId":"p7","node":"game-server-1"}

$ curl -i http://127.0.0.1:5180/locations/players/ghost
HTTP/1.1 404 Not Found
```

### 10. STREAM and the Session-Actor link

An external client attaches over TCP, linking the connector only.

```console
$ ./build/tutorial_stream_client
connected: true
round trip: 2ms          # STREAM request/reply
bound player: p1         # the connection is bound to a player
pushed: speedy           # the player pushes over that connection
```

`pushed` is the point: the client only sent a nickname change and received a push the player
sent on its own. In C++ every packet arrives through one `on_packet`, `reply_packet` answers
requests only (pushes use `bound_session().send(...)`), and the connector is opened with manual
dispatch so a push arriving before `wait` is queued rather than dropped.

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
| `location-find` | `Client/main.cpp` |
| `actor-contracts` | `Shared/contracts.hpp` |
| `actor-class` · `actor-factory` | `Server/actors/player.hpp` |
| `entry-spot` · `actor-handlers` · `actor-push` · `actor-send-handler` · `actor-request-handler` | `Server/spots/lobby_spot.hpp` |
| `actor-register` | `Server/main.cpp` |
| `actor-create-call` · `actor-send-call` · `actor-request-call` | `Client/main.cpp` |
| `stream-contracts` · `session-actor-contracts` | `Shared/contracts.hpp` |
| `session-class` · `session-handler` · `session-actor-bind` · `session-actor-relay` | `Server/sessions/game_session.hpp` |
| `stream-register` | `Server/main.cpp` |
| `stream-client` · `session-actor-client` | `StreamClient/main.cpp` |

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
