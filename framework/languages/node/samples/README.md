# ZLink Node.js Framework Samples

Seven samples that use the public API of the Node.js/NestJS framework. Build and run them from
this directory alone, without the repository (an unzipped `zlink-samples-node/`, or the
repository's `framework/languages/node/samples/`).

## Sample list

| Sample | What it shows | Client runtime | Wiring |
|---|---|---|---|
| `Bingo.Ts` | Session, Entry Spot, room Spot, player Actor, timers and bound-session push | Chromium | Redis location store |
| `TicTacToe.Ts` | Scale-out across two API and two Play processes, room lookup and live play | Chromium | Manual MeshNode peer and a Redis room route store |
| `SupportChat.Ts` | Conversation ownership, agent assignment, reconnect, idle timers and close notices | Chromium | Redis location store |
| `DeliveryDispatch.Ts` | Delivery dispatch, timeout reassignment and customer/courier status notices | Chromium | Redis location store |
| `GameQuest.Ts` | Per-player quest-owner Spot, event stream and a read model | Chromium | Redis location store |
| `ShoppingMall.Ts` | Order workflow, event stream, an HTTP read model and fanout events | Node.js HTTP client | Redis location store |
| `ZoneWorld` | Actor zone crossing, Logical Multicast, node-direct calls, runtime events and an ops UI | Headless Node.js scenario with its own Chromium client | Redis location store |

Only TicTacToe wires a MeshNode peer endpoint by hand. The other samples use the Redis location
store to find Spot and Actor placement and to wire up peers.

Language-neutral content — business flow, message contracts and the smoke sequence — is owned by
the repository's [common sample document](../../../doc/framework/common/sample/README.en.md) (only
reachable from inside the repository; not needed to run the samples).

## Prerequisites

- **Node.js 22 or newer.** `@zlink-systems/zlink@1.2.0` declares `"engines": { "node": ">=22" }`.
  Check with `node --version`.
- **Docker Desktop (or Docker Engine) running.** Nothing else. Each sample runner creates its own
  Redis container (`redis:7.2-alpine`) and removes it when it exits, so you never install or start
  Redis yourself.
- **Samples that need Chromium install Playwright inside their own directory.** All six samples
  except `ShoppingMall.Ts` (including `ZoneWorld`) run their client in a real Chromium — see the
  "Client runtime" column above. After `npm install`, run `npm run browser:install` once in that
  sample's directory. `ShoppingMall.Ts` uses a plain Node.js HTTP client, so it never needs this.

## Download and install

This directory is already the download — unzip `zlink-samples-node.zip` and it opens straight into
`Bingo.Ts/`, `TicTacToe.Ts/`, … the seven of them. There is no repository to clone. Each sample
resolves `@zlink-systems/*` from the npm registry at the version that sample pins. Samples are not
an npm workspace, so install each one separately.

```bash
cd Bingo.Ts
npm install
npm run browser:install   # only for samples that use Chromium (all but ShoppingMall.Ts)
```

Repeat with the other sample names in place of `Bingo.Ts` (`DeliveryDispatch.Ts`, `GameQuest.Ts`,
`ShoppingMall.Ts`, `SupportChat.Ts`, `TicTacToe.Ts`, `ZoneWorld`). Installing only the samples you
plan to check is fine.

Inside the repository, sharing the Node framework workspace, you can prepare everything at once
from the workspace root instead.

```bash
cd framework/languages/node
npm ci
npm run browser:install
```

## Build

There is no separate build step. Each sample's `npm run sample` (which `run_sample.*` calls)
runs `npm run build` automatically before it runs. To pre-build several samples at once on
Windows, use `build_samples.ps1`.

```powershell title="windows"
./build_samples.ps1 Bingo.Ts TicTacToe.Ts
# Omit the arguments to build all seven.
```

Linux/WSL has no separate build step — `run_sample.sh` in "Run" below builds it before running.

## Run

Each sample has a `run_sample.sh` (Linux/WSL) and `run_sample.ps1` (Windows); one invocation runs
one sample. Call it from this directory as-is.

```bash title="linux"
./Bingo.Ts/run_sample.sh
```

```powershell title="windows"
./Bingo.Ts/run_sample.ps1
```

Checking all seven means seven invocations. Put `DeliveryDispatch.Ts`, `GameQuest.Ts`,
`ShoppingMall.Ts`, `SupportChat.Ts`, `TicTacToe.Ts`, `ZoneWorld` in place of `Bingo.Ts` in turn to
run them one at a time (only `ZoneWorld` has no `.Ts` suffix). The detailed criteria for how a
run works are owned by the repository's
[common sample document](../../../doc/framework/common/sample/README.en.md), in the section on
sample run scripts and Redis isolation.

The runner generates each role's config file, starts the servers, starts its own Redis container,
waits for readiness, runs the client self-check, and cleans up — you never start Redis yourself.
For example, Bingo's full client flow can be seen in
[`Bingo.Ts/Client/bingo-client-scenario.ts`](Bingo.Ts/Client/bingo-client-scenario.ts).

## Verify

Every runner prints `PASS <Sample>` as the last line of standard output and exits `0` on success.

```
PASS Bingo.Ts
```

On failure there is no `PASS` line; the runner writes the failing role and the check that failed
to standard error and exits non-zero. Either way, it cleans up the servers, Chromium and Redis
container it started.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `docker: Cannot connect to the Docker daemon` | Docker Desktop (or `dockerd`) is not running. Start it and try again |
| `Package mode requires @zlink-systems/... Run npm install in ...` | You skipped `npm install` in that sample's directory. Do "Download and install" above first |
| `npm error gyp ERR! ... ZLINK_CORE_INSTALL_PREFIX must name an absolute installed Core ... package prefix` (Windows) | This version of `@zlink-systems/zlink` has no prebuild for this platform yet. Tracked separately (#656) — retry once a newer version is published |
| `browserType.launch: Executable doesn't exist` in a Chromium sample | You skipped `npm run browser:install` in that sample's directory |
| Port conflict (`EADDRINUSE`) | The runner picks a random loopback port each time, so this is rare unless you run the same sample twice at once. Retrying usually clears it |
| Playwright reports `Host system is missing dependencies` on Linux | Playwright needs system libraries that aren't installed. Run `npx playwright install-deps chromium` (needs admin rights) |
| `EBADENGINE` (Node version warning) | Node.js is older than 22. Upgrade per "Prerequisites" above |

## Where the documentation lives

Business flow, message contracts and the smoke sequence are owned by the repository's common
sample document. When a Node sample has no setup or run procedure that differs from the common
one, its own directory does not repeat a README.

## MeshNode and channels

One physical mesh is one MeshNode per process. A ChannelName is the logical service group that
MeshNode participates in, and it never creates a separate ROUTER endpoint. Node-direct calls,
ChannelName select-one, Spot, Actor and Logical Multicast all use the same MeshNode. Classic
fanout, which delivers to every subscriber, is a separate PUB/SUB channel.

## The browser client boundary

A client that uses the Stream Connector is built as a browser ESM bundle and runs in a real
Chromium. Node.js is responsible for producing the bundle, serving static files and driving
headless Chromium — it is never used as the Stream Connector's client runtime.

`ZoneWorld/Browser/` is this language's browser UI — inside the repository it is the same content
as the shared TypeScript source (`framework/languages/shared_sample/zoneworld/client/`) that every
language's server connects to, and this one directory is self-contained on its own. See the
repository's
[TypeScript Stream Connector guide](../../../doc/framework/node/guide/stream-connector/README.en.md)
for how to use the connector.
