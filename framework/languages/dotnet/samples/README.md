# ZLink Framework .NET Samples

.NET samples demonstrate the published `Zlink.Framework` packages, at the
version pinned in `Directory.Packages.props`, through separate server-role
processes and executable client scenarios. Their domain flows and
verification rules follow the
[common sample scenarios](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/README.ko.md).

This file only needs the .NET SDK, Docker, and (for six of the seven samples)
Python 3 -- no repository checkout. The Korean canonical version is
[README.ko.md](README.ko.md).

## Prerequisites

- **.NET SDK 8.0** -- `dotnet --version` reports an `8.0.x` SDK, on PATH.
- **Docker Desktop** (or another Docker Engine), running and reachable from
  the shell that starts a sample. Every `run_sample.sh`/`run_sample.ps1`
  starts and removes its own single-use Redis container (`redis:7.2-alpine`,
  ports 22000-22099) for that run -- nothing else needs Redis installed
  separately, and no container is left behind after a run finishes.
- **PowerShell** for the `.ps1` runners: Windows PowerShell 5.1 (built into
  Windows) or PowerShell 7 both work.
- **Python 3**, on PATH (or discoverable through the `py` launcher on
  Windows) -- needed only for some samples, on some platforms:
  - On Linux/WSL, `run_sample.sh` for Bingo, GameQuest, ShoppingMall,
    SupportChat, TicTacToe, and ZoneWorld needs it to generate role
    configuration files; DeliveryDispatch does not need Python at all.
  - On Windows, only ZoneWorld needs Python (`run_sample.ps1`'s ZW-B8
    fault-injection proxy, part of its default `all` scenario run); the
    other six `.ps1` runners never invoke Python.
- ZoneWorld's `-BrowserSmoke`/`--browser-smoke` flag additionally needs
  Node.js and npm, and only works from a full repository checkout (see
  [ZoneWorld](ZoneWorld)); it is off by default and not required for the
  seven-sample check below.

## Download and install

Each sample references the published `Zlink.Framework`/`Zlink.Stream.Connector`
NuGet packages (`nuget.config` in this directory points only at
`nuget.org`); nothing here needs a repository checkout, and `dotnet restore`
(run implicitly by `dotnet build`, below) fetches them the first time you
build a sample. Extract `zlink-samples-dotnet.zip` anywhere and run the
commands in this file from the extracted `zlink-samples-dotnet` directory
(a repository checkout runs the same commands from
`framework/languages/dotnet/samples`).

## Build

Each `run_sample.sh`/`run_sample.ps1` builds its own sample before running
it -- there is no separate build step to run first. To build one sample by
hand (for example to check it compiles without running it):

```bash title="linux"
dotnet build TicTacToe/TicTacToe.sln
```

```powershell title="windows"
dotnet build TicTacToe\TicTacToe.sln
```

## Run

Each sample root owns `run_sample.sh` and `run_sample.ps1`, and one invocation
runs one sample. The
[common sample document](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/README.ko.md)
owns this rule in its "The Sample Run Script And Redis Isolation Standard"
section; what follows is only the command for this language, run from this
`samples` directory (`framework/languages/dotnet/samples` in a repository
checkout, or the root of an extracted `zlink-samples-dotnet.zip`). The runner
starts its own Redis container in Docker itself -- do not start one by hand.
This saves the run's own output to a file the next section checks.

```bash title="linux"
set -o pipefail
./TicTacToe/run_sample.sh 2>&1 | tee tictactoe-run.log
```

```powershell title="windows"
.\TicTacToe\run_sample.ps1 *>&1 | Tee-Object -FilePath tictactoe-run.log
if ($LASTEXITCODE -ne 0) { throw "run_sample.ps1 failed with exit $LASTEXITCODE" }
```

There are seven .NET samples, so checking them all takes seven invocations.
Substitute `Bingo`, `DeliveryDispatch`, `GameQuest`, `ShoppingMall`,
`SupportChat`, `TicTacToe`, and `ZoneWorld` (and each one's own completion
marker below) in turn, one at a time.

The runner creates
role-specific configuration files, starts each role as a separate process,
waits for readiness, runs the probe or client self-check, and then removes the
processes and Redis container it created. Server code starts only its own role.

## Verify

A successful run builds, runs every scenario, tears every role down cleanly,
and prints that sample's completion marker as its last line before exiting
`0`:

| Sample | Marker |
|---|---|
| TicTacToe | `tictactoe-placement=completed` |
| Bingo | `bingo-placement=completed` |
| SupportChat | `supportchat-placement=completed` |
| ShoppingMall | `shoppingmall-placement=completed` |
| DeliveryDispatch | `deliverydispatch-placement=completed` |
| GameQuest | `gamequest-placement=completed` |
| ZoneWorld | `zoneworld=completed` |

For TicTacToe, that means `tictactoe-run.log` from "Run" above ends with
`tictactoe-placement=completed`:

```bash title="linux"
grep -q 'tictactoe-placement=completed' tictactoe-run.log
```

```powershell title="windows"
if (-not (Select-String -Path tictactoe-run.log -Pattern 'tictactoe-placement=completed' -Quiet)) {
  throw "tictactoe verify failed"
}
```

A nonzero exit code, or any line starting `scenario ... FAILED` /
`!! ... withheld`, means the run did not pass; the runner still cleans up its
processes and Redis container either way.

## Troubleshooting

- **Docker is not running** -- `run_sample.sh`/`run_sample.ps1` fails
  immediately with a message that Docker is required. Start Docker Desktop
  (or your Docker Engine) and retry.
- **`Could not find N free ports` / a role fails to bind its endpoint** --
  something else on the machine is using a port in the sample's ephemeral
  ranges (22000-22099 for the run's Redis container, 22100-23999 for role
  endpoints). Close whatever is using them, or just retry -- the runner picks
  a new random set of ports each time.
- **`dotnet` reports no compatible SDK** -- install the .NET 8.0 SDK; a newer
  major SDK alone is not enough unless it still carries an `8.0.x` runtime.
- **Python 3 is required** (Linux/WSL, or ZoneWorld on Windows) -- see
  Prerequisites above; install Python 3 and make sure `python3` (Linux/WSL)
  or `python`/`py -3` (Windows) resolves to a real interpreter, not a Windows
  Store alias stub.
- A sample container or process left behind after a crashed run -- every
  container this zip creates is named `zlink-<sample>-dotnet-redis-*`; remove
  it with `docker rm -f` if a run was interrupted (Ctrl-C, killed shell)
  before its own cleanup ran.

## Samples

| Sample | Purpose | Peer topology |
|---|---|---|
| [TicTacToe](TicTacToe) | Two API roles and two Play roles demonstrate room lookup, Actor turns, and real-time game messages. | Manual MeshNode peers; Redis room route store |
| [Bingo](Bingo) | Session admission, Entry and room Spots, Actor binding, timer draws, and bound-session notifications. | Redis location store |
| [SupportChat](SupportChat) | API, Support, and Session roles demonstrate conversation ownership, reconnect, idle timeout, and close notifications. | Redis location store |
| [ShoppingMall](ShoppingMall) | Commerce API and order workflow roles demonstrate event-sourced orders, projections, and fanout events. | Redis location store |
| [DeliveryDispatch](DeliveryDispatch) | Dispatch, courier, tracking, and customer gateway roles demonstrate timeout reassignment and session push. | Redis location store |
| [GameQuest](GameQuest) | Session and player quest owner roles demonstrate event-sourced quest progress and projections. | Redis location store |
| [ZoneWorld](ZoneWorld) | Gateway, ZoneNode, and Ops roles demonstrate Actor transfer, zone Logical Multicast, Node direct operations, runtime events, and browser visualization. | Redis location store |

TicTacToe is the only sample that configures MeshNode peers manually. Every
other sample uses the Redis location store to resolve Spot and Actor locations
and establish MeshNode peers.

## MeshNode And Channel Names

Each physical mesh has one MeshNode per process. `ChannelName(...)` adds logical
service membership to that MeshNode and does not create another ROUTER endpoint.
Node direct, ChannelName select-one, Spot, Actor, and Logical Multicast operations
share the MeshNode. Classic fanout remains a separate PUB/SUB channel.

```csharp
var mesh = options.AddRouteMesh("game")
    .Listen("tcp://0.0.0.0:7300"); // Creates this process's MeshNode endpoint.

mesh.ChannelName("orders"); // Adds logical service membership without another ROUTER.

options.AddFanoutChannel("events")
    .EnablePublisher("tcp://0.0.0.0:7400"); // Classic fanout uses its own PUB endpoint.
```

## Configuration And Contracts

Framework hosts bind endpoint, Redis, routing ID, timeout, and logging settings
from role-specific configuration files and pass typed settings to
`AddZLinkFramework(...)`. Application code does not read those values directly
from environment variables. A standalone client accepts only the external
endpoint and scenario options it must know through validated command-line
arguments or its own configuration file.

Shared projects contain only message contracts serialized by both client and
server roles. Server topology and framework settings belong under
`Server/Configuration`; client and probe settings belong to their respective
projects.

For a manual TicTacToe run, give each role its own configuration file:

```bash
dotnet run --project TicTacToe/Server.Play -- --config ./appsettings.play-a.json
dotnet run --project TicTacToe/Server.Play -- --config ./appsettings.play-b.json
dotnet run --project TicTacToe/Server.Api -- --config ./appsettings.api-a.json
dotnet run --project TicTacToe/Server.Api -- --config ./appsettings.api-b.json
```
