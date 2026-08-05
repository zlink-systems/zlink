<!-- framework-adapter-nav:start -->
[Document list](../../../README.en.md) | [Common spec](../README.en.md) | [Scenario E2E](../e2e/README.en.md)
<!-- framework-adapter-nav:end -->

# Framework Performance Test Common Specification

This document is the common specification for making per-language performance tests of the ZLink
Framework consistent with one another. The performance tests described here don't define a new
public API contract. They're the execution standard for measuring performance, using the framework
public API and the stream connector public API each language already provides, on the same server
configuration and the same message flow.

Performance tests don't replace functional verification. Whether a feature is correct is confirmed
first by contract, sample, and e2e tests. On top of that, performance tests reliably measure
throughput, latency, resource usage, and failure rate. So the test code doesn't bypass features or
directly call internal runtime APIs.

## 1. Goals

The goal of performance testing isn't to get a single number — it's to get a result that can
explain which layer is the bottleneck. Every language must be able to answer the questions below.

- How do throughput and latency change as the stream connector handles many client connections?
- How much does the cost differ when session and actor are on the same server versus different
  servers?
- What's the difference between the request/reply style and the send/send style in server-to-server
  channel and Spot messaging?
- How much does the Framework's automatic turn management reduce head-of-line blocking when a Spot
  handler waits for a remote request's `Async` completion?
- When a Spot handler offloads work to the local worker pool with `runCpuWorker(...)` and waits for
  completion, how does turn management affect queue progress and continuation-resume cost?
- How do throughput and latency change as payload size grows from 1 KiB to 4 KiB?
- Can the bottleneck candidate — client runner, server process, framework dispatch, codec, or
  transport — be isolated with evidence?

## 2. Scope

The common performance tests are implemented with the same meaning in the `.NET`, Java, Kotlin,
Node.js, and C++ frameworks. Per-language syntax and runner tools can differ, but the scenario
names, payload sizes, measurement windows, metric names, and success criteria match.

The initial scope is the seven axes below. Every standard scenario runs at two payload sizes,
`1 KiB` and `4 KiB`. However, when interpreting results, the CS (session/actor) family, the Spot
execution family, and the PS (publish-subscribe) family are read at `1 KiB` as the representative
figure, while the S2S (channel/Spot) and AC (actor client no-bind) families are read at `4 KiB`.

| Axis | Purpose | Representative Payload | Payload Run Alongside |
|----|------|---------------|--------------------|
| connector → session → actor local echo | Measures client/server boundary and same-server actor dispatch cost | 1 KiB | 4 KiB |
| connector → session → remote actor echo | Measures the cost of a structure where the session server and actor server are separated | 1 KiB | 4 KiB |
| channel → remote Spot echo | Measures the cost of requesting or sending from a server-to-server channel to a remote Spot | 4 KiB | 1 KiB |
| remote Spot → channel echo | Measures the cost of requesting or sending from a remote Spot to a channel server | 4 KiB | 1 KiB |
| Spot worker echo | Measures the cost of a Spot handler's remote-request `async` wait and `runCpuWorker(...)` local-worker-pool offload wait, and queue progress | 1 KiB | 4 KiB |
| actor client(no-bind) → actor echo | Measures the cost of a session-less server-side caller sending/requesting directly to an `ActorRef` | 4 KiB | 1 KiB |
| publish → subscriber fanout | Measures fanout throughput and delivery latency when one publisher broadcasts an event to multiple subscribers | 1 KiB | 4 KiB |

The `publish → subscriber fanout` axis is based on the `publish-subscribe` common interaction model
defined by §3.3 of `framework/doc/framework/common/spec/03-interaction-model.en.md` and
`framework/doc/framework/common/spec/07-channel-topology.en.md`, and on the fanout scenario of
`framework/doc/framework/common/e2e/config-3-pubsub.en.md`. Since `publish-subscribe` is a public
contract already implemented by all five languages, this axis is mandatory the same as the other
five axes, with no per-language skip.

The `actor client(no-bind) → actor echo` axis is based on the actor client contract in §6.1 of
`framework/doc/framework/common/spec/14-actor-model.en.md` and the
`framework/doc/framework/common/e2e/config-9-to-actor-messaging.en.md` scenario. Every framework
language that provides a public actor client keeps this axis as the same mandatory perf scope.

Payload size means the size of the message payload body. It doesn't include the framework header,
connector frame, or codec metadata. Every language must produce the same byte pattern. So that
compression or string-interning effects don't distort the result, the payload isn't filled with one
repeated character — it's filled with a fixed seed or index pattern. The common standard run only
requires `1 KiB` and `4 KiB`. Smaller or larger messages can be added for ad-hoc analysis, but
aren't part of the common perf scope every language must implement.

## 3. The Relationship Between Performance Tests And Other Tests

| Test Kind | Main Purpose | Process Boundary | Result Interpretation |
|-------------|---------|---------------|-----------|
| unit/contract | Correctness of the API and small behaviors | Mostly in-process | Fast regression detection |
| sample | A normal flow a user can follow | Real server and client | Public usage verification |
| e2e | Verifies deployment shape and failure paths | Real multi-process | Feature-combination verification |
| perf | Measures throughput, latency, resource usage | Real multi-process | Bottleneck candidates and regression tracking |

perf doesn't reuse a sample or e2e server. Since the performance test server must have a clear
measurement target, it's kept as an echo server with minimal domain rules. However, the server
configuration follows the actual framework structure verified in samples. For example, a local
session/actor structure is TicTacToe-shaped, and a remote session/actor structure is Bingo-shaped.

## 4. Common Execution Model

Every perf runner runs the phases below in the same order.

| Phase | Name | Description | Included In Metrics |
|-------|------|------|------------------|
| 1 | build/preflight | Release build, port reservation, OS limit check, log directory preparation | Excluded |
| 2 | server start | Starts the needed server processes and confirms readiness | Excluded |
| 3 | connect | Creates the client connector and finishes authentication or session preparation | Recorded separately, excluded from throughput calculation |
| 4 | warmup | Warms up JIT, codec cache, connection path, and pools | Excluded |
| 5 | reset | Resets client/server metrics at the same time | Excluded |
| 6 | measured | Applies real load for the duration | Included |
| 7 | settle | Waits for the last response and server metric reflection | Excluded from throughput calculation |
| 8 | report | Collects client/server metrics and writes result files | Excluded |
| 9 | cleanup | Terminates the server and client processes | Excluded |

Benchmarks are duration-based by default. Message-count-based tests are kept only for short smoke
runs and debugging. A duration basis makes it easier to compare per-language runtime warmup
differences and distributed client execution.

## 5. Common CLI

Each language runner provides options with the same meaning. Option names can be adapted to
per-language convention, but the shell runner must support the following long options.

| Option | Default | Meaning |
|------|--------|------|
| `--scenario` | required | The scenario name to run |
| `--connections` | `10000` | Total number of connector clients |
| `--client-index` | `0` | This runner's index among multiple load generators |
| `--client-count` | `1` | Total number of load generators |
| `--duration-seconds` | `30` | Duration of the measured phase |
| `--warmup-seconds` | `5` | Duration of the warmup phase |
| `--payload-size` | scenario representative value | Single payload byte size |
| `--payload-sizes` | `1024,4096` | Runs multiple payload sizes in order |
| `--inflight` | `1` | Concurrent requests or incomplete echoes per client |
| `--connect-concurrency` | `256` | Number of connectors attempting connection at once |
| `--spot-count` | `16` | Number of Spot RIDs to distribute load across in Spot execution scenarios. `spot-await-contention` fixes this to `1` regardless of this value |
| `--subscriber-count` | `8` | Number of subscriber processes receiving fanout in pub/sub scenarios |
| `--worker-task-millis` | `5` | The fixed-cost CPU work duration `runCpuWorker(...)` performs in the Spot worker offload scenario |
| `--worker-pool-size` | `8` | The maximum thread count of the framework worker pool in the Spot worker offload scenario |
| `--mode` | scenario default | One of `request`, `send-send`, `async-request`, `no-await`, `publish`, `worker-offload` |
| `--codec` | `json` | Payload codec. Isn't overridden if the scenario fixes it |
| `--output` | `perf-results/<run-id>` | Result file directory |
| `--run-id` | timestamp | Run id that groups logs and results |
| `--endpoint-config` | generated by the script | JSON file holding the app endpoint and metrics endpoint per server role |

`--client-index` and `--client-count` are required options to avoid a situation where all 10,000
connectors pile onto one process and the client runner becomes the bottleneck. For example, with
`--connections 10000 --client-count 4`, each runner handles 2,500 connectors. If it doesn't divide
evenly, the lower indices each take one extra.

`run_perf.sh` uses `--payload-sizes 1024,4096` by default. `run_single.sh` supports running a single
`--payload-size` value for debugging convenience. Standard result comparisons are recorded
separately per payload size, and KOPS across different payload sizes aren't averaged into one line.

`--endpoint-config` is generated by the runner script after it brings up the server processes. The
client runner and the server trigger runner look only at this file to find the per-role endpoint.
In a multi-role scenario, listing multiple endpoint options on the command line tends to produce
different names per language, so the common input is fixed to a single JSON file.

```json
{
  "runId": "20260626-123000",
  "roles": {
    "sessionActorLocal": {
      "appEndpoint": "tcp://127.0.0.1:21001",
      "metricsUrl": "http://127.0.0.1:31001"
    },
    "session": {
      "appEndpoint": "tcp://127.0.0.1:21002",
      "metricsUrl": "http://127.0.0.1:31002"
    },
    "actor": {
      "appEndpoint": "tcp://127.0.0.1:21003",
      "metricsUrl": "http://127.0.0.1:31003"
    },
    "channel": {
      "appEndpoint": "tcp://127.0.0.1:21004",
      "metricsUrl": "http://127.0.0.1:31004"
    },
    "spot": {
      "appEndpoint": "tcp://127.0.0.1:21005",
      "metricsUrl": "http://127.0.0.1:31005",
      "spotRids": ["perf-spot-0", "perf-spot-1", "perf-spot-2", "perf-spot-3"]
    },
    "remoteEcho": {
      "appEndpoint": "tcp://127.0.0.1:21006",
      "metricsUrl": "http://127.0.0.1:31006"
    },
    "publisher": {
      "appEndpoint": "tcp://127.0.0.1:21008",
      "metricsUrl": "http://127.0.0.1:31008"
    },
    "subscribers": [
      { "appEndpoint": "tcp://127.0.0.1:21101", "metricsUrl": "http://127.0.0.1:31101", "subscriberId": 0 },
      { "appEndpoint": "tcp://127.0.0.1:21102", "metricsUrl": "http://127.0.0.1:31102", "subscriberId": 1 }
    ],
    "registry": {
      "appEndpoint": "tcp://127.0.0.1:21007",
      "metricsUrl": "http://127.0.0.1:31007"
    }
  }
}
```

Roles that aren't needed are omitted. `appEndpoint` is that role's framework communication
endpoint, and `metricsUrl` is an HTTP endpoint dedicated to performance testing. When a scenario
must tell the server to start the benchmark, it sends a `PerfTriggerRequest` to that role's
`appEndpoint`. The HTTP metrics endpoint isn't used as a trigger path.

Unlike `spotRids`, `subscribers` isn't a list inside one role — it's independent process roles
listed as an array. The array length equals `--subscriber-count`. Each entry is a separate process
with its own `metricsUrl`, so `/perf/reset`/`/perf/stats` are called separately per subscriber and
results are also kept per subscriber (extending the §15 `server-<role>.json` convention to
`server-subscriber-<subscriberId>.json`).

The `spot` role's `spotRids` is a list of Spot RIDs the Spot server has already created, and its
length equals `--spot-count`. The client divides this list across connectors in order, spreading
requests across multiple Spot RIDs. `spot-await-contention` forces `--spot-count 1`, so it uses
only the first value of this list, and every connector sends requests to the same Spot RID. A
scenario that spreads load across multiple Spot RIDs and a scenario that concentrates on a single
Spot RID must be distinguished by this list length alone — a per-language implementation must not
arbitrarily decide the RID count.

## 6. Standard Project Structure

The perf project is kept separate from sample or e2e. Since a performance test's measurement path
must be simple, it doesn't reuse a sample's domain code or e2e's failure-scenario code. Instead,
like the PlayHouse benchmark, it splits server, client, shared, and metrics into separate execution
projects, and the runner script brings up and cleans up the processes.

Even if the actual per-language build file names differ, the logical structure follows the layout
below.

```text
framework/languages/<lang>/perf/
|-- README.ko.md
|-- Shared/
|   |-- Contracts/          echo request/reply, trigger, metrics DTO
|   `-- Payload/            payload generation rules and validation helpers
|-- Client/
|   |-- Program.*           only handles CLI parsing and scenario selection
|   |-- Scenarios/
|   |   |-- CsLocalSessionActorEchoScenario.*
|   |   |-- CsRemoteSessionActorEchoScenario.*
|   |   |-- S2sChannelToSpotRequestEchoScenario.*
|   |   |-- S2sChannelToSpotSendSendEchoScenario.*
|   |   |-- S2sSpotToChannelRequestEchoScenario.*
|   |   |-- S2sSpotToChannelSendSendEchoScenario.*
|   |   |-- SpotAsyncRequestEchoScenario.*
|   |   |-- SpotAwaitContentionScenario.*
|   |   |-- SpotNoAwaitEchoScenario.*
|   |   |-- SpotWorkerOffloadEchoScenario.*
|   |   |-- ActorNoBindRequestEchoScenario.*
|   |   |-- ActorNoBindSendSendEchoScenario.*
|   |   `-- PubSubFanoutEchoScenario.*
|   |-- Support/
|   |   |-- PerfClientOptions.*
|   |   |-- PerfRunPlan.*
|   |   |-- ConnectionPool.*
|   |   |-- CorrelationTable.*
|   |   |-- InFlightLimiter.*
|   |   |-- MetricsClient.*
|   |   |-- ClientMetricsCollector.*
|   |   |-- ResultWriter.*
|   |   `-- ScenarioRunner.*
|   `-- README.ko.md
|-- Servers/
|   |-- SessionActorLocal/
|   |-- Session/
|   |-- Actor/
|   |-- Channel/
|   |-- Spot/
|   |-- RemoteEcho/
|   |-- ActorCaller/        a session-less server that calls the actor client
|   |-- Publisher/
|   |-- Subscriber/
|   `-- Registry/           only in languages/scenarios that need a registry
|-- ServerSupport/
|   |-- Metrics/
|   |-- Readiness/
|   |-- Logging/
|   |-- Payload/
|   `-- ProcessMetrics/
|-- scripts/
|   |-- run_perf.sh
|   |-- run_single.sh
|   `-- collect_env.sh
`-- perf-results/           gitignore target
```

Because of per-language naming convention, `shared`, `client`, `servers` can be used instead of
`Shared`, `Client`, `Servers`. However, keep one convention within a single language, and use the
logical names above in common documents and result files.

### 6.1 Top-Level Responsibilities

| Location | Responsibility | Prohibited |
|------|------|-----------|
| `Shared/` | Message contract, payload generation rules, and result DTO shared by client/server | Server host configuration, framework handlers, client runner logic |
| `Client/` | CLI, scenario execution, connector creation, warmup/measured phase, saving results | Directly implementing the server process, calling framework internal APIs |
| `Client/Scenarios/` | Per-scenario connector public-call flow and verification criteria | Common option parsing, metrics-endpoint implementation detail |
| `Client/Support/` | Options, run plan, connection pool, metrics client, result writer | A helper that wraps connector calls and hides scenario meaning |
| `Servers/<Role>/` | The per-role execution server and benchmark handlers | Switching multiple server roles with a single `--role` |
| `ServerSupport/` | Metrics, readiness, logging, payload validation shared by roles | Business echo flow, role-specific handlers |
| `scripts/` | Build, preflight, server start, client start, cleanup | Performance-measurement hot-path logic |

Keep `Program.*` thin. It only does CLI parsing, logging initialization, DI/host factory calls, and
scenario selection. An entry point that reads options and calls the runner, like PlayHouse
benchmark's `Program.cs`, is fine, but the measured-phase loop or echo-completion aggregation must
not go into `Program.*`.

### 6.2 The 10,000-Client Driving Model

10,000 connector clients are handled as the client runner's standard execution model, not as one
big helper. Like the PlayHouse benchmark, the runner creates a connector array or connection pool,
finishes connect/auth with bounded concurrency, then hands the same pool to the warmup and measured
phases.

The basic structure is as follows.

```text
Client Program
  -> parse options
  -> build PerfRunPlan
  -> create ScenarioRunner
  -> run selected Scenario

ScenarioRunner
  -> reserve client id range
  -> create ConnectionPool
  -> connect/auth with max connect concurrency
  -> run warmup on a bounded subset or all connections
  -> reset client/server metrics
  -> run measured operation on every connected connector
  -> collect metrics and write result
  -> disconnect all connectors
```

`ConnectionPool` computes the client id range this process handles, based on `connections`,
`clientIndex`, and `clientCount`. For example, if the full 10,000 connectors are split across 4
load generators, each process creates roughly 2,500 connectors. `clientId` must be kept as a global
id — so that correlation ids and error logs don't collide even when merging results from multiple
load generators.

Connections aren't attempted 10,000 at once. They're limited to `--connect-concurrency 256` by
default, and each connector retries up to about 3 times. A connection failure isn't hidden — it's
recorded in `connections.failed`. The measured phase uses only successfully connected connectors.
However, if the connection success rate is too low for the result to be meaningful, the runner
must fail. The default failure threshold is a connection success rate below 99%.

Warmup isn't mixed into measured results. The warmup connection count can be limited to a range the
implementation language can handle, and if limited, `warmupConnections` is recorded in the result
config. An implementation where callback state can linger after warmup — request callbacks, push
waits, send/send — must recreate the connection after warmup or fully clear pending state, like the
PlayHouse benchmark does.

In the measured phase, every connector runs the same operation loop. The request family keeps up to
`inflight` incomplete requests per client, and the send/send family limits the number of incomplete
entries in the correlation table to `inflight`. In a language whose connector implementation
requires an explicit dispatch pump, the operation loop periodically calls the public dispatch/poll
API. This call must also be a connector public API — it must not directly call the runtime's
internal pump.

To confirm whether the client runner is the bottleneck, each client process records its own CPU
usage, event loop delay, or a per-language runner state like thread pool queue depth. A run where
the client process's CPU is saturated isn't interpreted as a server performance limit.

### 6.3 Server Role Separation

If server roles differ, they're kept as separate execution projects. Adding a role-switching option
like `--role session`, `--role actor`, `--role spot` to one binary improves execution convenience,
but blurs the real deployment process boundary and the performance result.

| Server | What It Includes | Measurement Path |
|------|-------------|-----------|
| `SessionActorLocal` | Stream session, local actor, actor factory, local echo handler | connector → session → actor |
| `Session` | Stream session, remote actor relay, request-reply handling | connector → session → remote actor |
| `Actor` | Actor/Entry Spot or actor-owner Spot, echo actor handler | Remote actor echo, actor client no-bind echo |
| `Channel` | Channel request/send handler, trigger endpoint | channel ↔ Spot |
| `Spot` | Spot factory, Spot handler, a perf-only timer if a timer is needed | Spot ↔ channel, automatic turn |
| `RemoteEcho` | The plain channel echo server the Spot handler calls in Spot execution scenarios | Remote request under automatic turn management |
| `ActorCaller` | An external caller that doesn't create a session, executing the actor client's `SendToActor`/`RequestToActor` calls | Actor client no-bind echo |
| `Publisher` | Publish channel server, publishing events with the public `EventPublish`/`Publish(...).Async()` API | Publish fanout |
| `Subscriber` | Subscribe handler, records incoming events as evidence/metrics | Publish fanout |
| `Registry` | Registry for configurations that need discovery | Not on the measured path |

Each server process has its own metrics endpoint. If multiple servers share one metrics endpoint,
which role is the bottleneck can't be isolated.

Scenarios `ActorCaller` participates in need an official location store (e.g. Redis) extension to
resolve actor location. In this configuration, instead of a `Registry` role, the location store
itself is brought up as a shared dependency, and its per-role endpoint is recorded in the
endpoint-config.

`Publisher`/`Subscriber` also need the same location store for the fanout connection (same as
config-3-pubsub). `Subscriber` is brought up as `--subscriber-count` separate processes — one
binary doesn't simulate multiple instances with `--role subscriber`.

### 6.4 Separating Support Code

Support code exists to lower the complexity of the measurement apparatus — not to hide the
scenario's core flow. The client scenario's body should be composed only of connector public API
calls where possible. Since the connector surface already sufficiently expresses intent, a helper
that re-wraps request/send/wait calls isn't created by default.

Good separation:

- `ConnectionPool` handles creating 10,000 connectors, connection retries, and limiting connect
  concurrency.
- `ScenarioRunner` handles phase order and cancellation.
- `ClientMetricsCollector` records latency and counters.
- `MetricsClient` handles `/perf/reset`, `/perf/stats`, `/perf/ready` calls.
- `ResultWriter` writes `result.json`, `summary.txt`, and per-role metric files.

Bad separation:

- A helper like `RunAllBenchmarkLogic(...)` that hides the whole scenario flow.
- A helper like `SendMagicEcho(...)`, `RequestEchoAsync(...)`, `WaitEchoReply(...)` that re-wraps a
  connector public call and hides the request/send/send-send distinction from the call site.
- A helper that puts a server role's framework calls into client support.
- A helper that takes a runtime-internal object instead of the public API to build a fast path.

## 7. Folder Structure Rules

A folder is created only when a responsibility repeats. A `Utils/`, `Common/`, or `Infrastructure/`
folder just to hold one file isn't created. Since a performance test reader must quickly find the
measurement path, folder names must directly reveal role and responsibility.

### 7.1 Client Folder

Files under `Client/Scenarios/` map 1:1 to scenario names. For example,
`s2s-channel-to-spot-send-send-echo` goes into the `S2sChannelToSpotSendSendEchoScenario.*` file.
Multiple scenarios aren't merged into one file.

`Client/Support/` holds only the following kinds of files.

| File | Role |
|------|------|
| `PerfClientOptions.*` | Converts CLI options into typed configuration |
| `PerfRunPlan.*` | Splits the total connections by client-index/client-count |
| `ConnectionPool.*` | Connector creation, authentication, retries, cleanup |
| `CorrelationTable.*` | Aggregates send/send completion by correlation id |
| `InFlightLimiter.*` | Limits per-client incomplete work for requests and send/send |
| `ScenarioRunner.*` | Runs phase order |
| `ClientMetricsCollector.*` | Collects client latency/counters |
| `MetricsClient.*` | Calls the server metrics endpoint |
| `ResultWriter.*` | Writes result files |
| `PayloadFactory.*` | Generates and validates payload patterns. Doesn't include connector calls |

### 7.2 Server Folder

Each `Servers/<Role>/` is an independent execution project. The internal structure of a role starts
small.

```text
Servers/Spot/
|-- Program.*
|-- SpotServerHostFactory.*
|-- SpotPerfOptions.*
|-- EchoSpot.*
|-- Handlers/
|   |-- SpotEchoRequestHandler.*
|   `-- SpotEchoSendHandler.*
`-- README.ko.md
```

If there are only one or two handlers, they can be placed at the server root without a `Handlers/`
folder. A folder is only created once handlers grow numerous and request, send, trigger, and metrics
start to mix. The metrics endpoint uses the common `ServerSupport/Metrics`, but per-role endpoint
registration is done directly by the role server.

### 7.3 Result And Log Folders

`perf-results/`, `logs/`, `tmp/` are gitignore targets. The runner creates a unique `run-id`
subfolder for each run. Overwriting a previous run's result in the same folder is forbidden because
it makes comparison difficult.

## 8. Client Scenario Writing Style

A client scenario is written so the real user flow is visible, like a sample's `ClientScenario`.
Like the PlayHouse benchmark's runner, the common runner handles connect, warmup, and measured
phases, but "what to send" and "what counts as complete" must be visible in the scenario file. In
particular, if the connector already expresses the request, send, wait, and dispatch flow well
through its public API, that call is used as-is.

### 8.1 The Top Of A Scenario File

The top of each scenario file briefly states the following.

- The server configuration this scenario measures.
- What one measured operation means.
- Whether it's a request style or a send/send style.
- The default payload size.
- Which metric records failure.

Comments explain measurement meaning, not repeat what the code does. For example, write "counts one
client-visible echo completion as one KOPS operation," not "sends a request."

### 8.2 Scenario Body

The scenario body keeps the following flow.

1. Get the connector pool, payload factory, and metrics collector from a `ScenarioContext` or
   equivalent object.
2. Define the operation used in warmup.
3. Define the operation used in the measured phase.
4. Record the operation-completion-confirmation criteria to metrics.
5. Validate the payload size and byte pattern.

A request scenario is kept so the request and response are visible within one function. Each line
of the flow below should map to an actual per-language connector public call or something close to
it.

```text
send PerfEchoRequest
await PerfEchoReply
verify payload
record completion latency
```

A send/send scenario is kept so correlation registration, send, reply receipt, and timeout handling
are visible.

```text
register correlation
send PerfEchoRequest
on PerfEchoReply:
  complete correlation
  verify payload
  record completion latency
expire old correlations:
  record timeout
```

Hiding this flow behind one name like `EchoAsync(...)` makes it hard to review the difference
between request and send/send. To reduce repetition, only pull out narrow responsibilities like
"payload generation," "latency recording," or "correlation table" as helpers.

### 8.3 Helper Usage Criteria

A helper isn't used to shorten a connector call. The following must be directly visible in the
scenario body.

- Which connector sends which packet/message.
- Whether it's request or send.
- Whether it awaits the reply, or receives it via a push/wait callback.
- When the correlation id is registered and when it's completed.
- What unit failures are timed out in.

The only allowed helpers are ones that don't hide the measurement flow.

| Allowed | Reason |
|------|------|
| Generating a payload byte pattern | Payload generation rules are shared by every scenario |
| Calling a latency recorder | How measurements are stored isn't scenario meaning |
| Percentile calculation | Result aggregation logic |
| Preparing the connection pool | Creating 10,000 connectors is a pre-measurement phase |
| The server metrics HTTP client | Not the measured path — it's a reset/snapshot tool |

The forbidden helpers are as follows.

| Forbidden | Reason |
|------|------|
| `EchoAsync(connector, payload)` | Hides the request/send/wait difference |
| `SendAndWait(...)` | Send/send correlation and timeout policy aren't visible |
| Putting the entire connector call inside `RunScenario(...)` | Only the scenario file's name remains — the measured flow disappears |
| Combining server trigger and completion aggregation into one helper | The client-visible operation's definition becomes unclear |

When repeated connector calls get long, first check whether the connector API is deep enough. If
it's hard to read through the public connector surface, don't hide it with a perf helper — split it
into a separate design issue about whether the framework/connector public API needs improvement.

### 8.4 Naming

The scenario class or file name follows the canonical names below.

| Scenario | File/Class Name |
|----------|-----------------|
| `cs-local-session-actor-echo` | `CsLocalSessionActorEchoScenario` |
| `cs-remote-session-actor-echo` | `CsRemoteSessionActorEchoScenario` |
| `s2s-channel-to-spot-request-echo` | `S2sChannelToSpotRequestEchoScenario` |
| `s2s-channel-to-spot-send-send-echo` | `S2sChannelToSpotSendSendEchoScenario` |
| `s2s-spot-to-channel-request-echo` | `S2sSpotToChannelRequestEchoScenario` |
| `s2s-spot-to-channel-send-send-echo` | `S2sSpotToChannelSendSendEchoScenario` |
| `spot-async-request-echo` | `SpotAsyncRequestEchoScenario` |
| `spot-await-contention` | `SpotAwaitContentionScenario` |
| `spot-no-await-echo` | `SpotNoAwaitEchoScenario` |
| `spot-worker-offload-echo` | `SpotWorkerOffloadEchoScenario` |
| `actor-no-bind-request-echo` | `ActorNoBindRequestEchoScenario` |
| `actor-no-bind-send-send-echo` | `ActorNoBindSendSendEchoScenario` |
| `pubsub-fanout-echo` | `PubSubFanoutEchoScenario` |

Per-language casing can change, but the words don't change. For example, a C++ file name can be
written `s2s_channel_to_spot_request_echo_scenario.cpp`.

### 8.5 What A Client Must Never Do Directly

- Don't create the server framework host in the same process.
- Don't directly call server handlers.
- Don't directly reference the server's internal metric collector object.
- Don't use a channel/spot client to bypass the server app endpoint. Even in server-to-server
  performance scenarios, the client only sends the trigger request — the measured server-to-server
  calls are executed by the server's internal handlers.
- Don't judge success by parsing console text. Judge it by echo reply, correlation completion, or
  metrics endpoint results.

## 9. Runner Script Writing Style

Each language has at least two scripts.

| Script | Purpose |
|--------|------|
| `run_single.sh` | Quickly runs one scenario and one mode during development |
| `run_perf.sh` | Runs the full set of standard scenarios in order and collects results under one run-id |

Like the PlayHouse benchmark, the script prints the following work as clear steps.

1. Release build.
2. Clean up existing perf server processes.
3. Reserve free ports and run OS preflight.
4. Start server processes.
5. Confirm readiness via `/perf/ready`.
6. Run the client runner.
7. Print the result file location.
8. Terminate server processes and clean up logs.

The script doesn't implement the measured path. The measured loop must live in the client scenario
and runner code.

When running multiple client runners at once, the script passes a different `--client-index` to
each process, and has every process use the same `--run-id` and the same server endpoint list.
Ports are reserved at run start and recorded in `config.json`. After the client processes finish,
all `client-<index>.json` files are read to produce an aggregated result. Aggregation only adds
values that can be summed, like counts and byte counts — latency percentiles are recalculated by
merging histogram buckets. If there's no histogram, or bucket upper bounds differ, the runner must
fail.

## 10. Standard Scenarios

### 10.1 `cs-local-session-actor-echo`

The client connector sends a `request` to the server's stream session, and the session delivers an
echo request to an actor in the same process. The actor returns the received payload as-is to the
session, and the session responds to the client as the reply of the same request.

| Item | Value |
|------|----|
| Server configuration | 1 `SessionActorLocalServer` |
| Client count | Default 10,000 |
| Payload | 1 KiB representative, 4 KiB alongside |
| Mode | `request` |
| Measurement unit | Client-visible echo completion |
| Comparison purpose | Connector + session dispatch + local actor dispatch cost |

This scenario is a simplified TicTacToe-shaped structure. There's no separate session gateway —
session and actor are in the same server process.

### 10.2 `cs-remote-session-actor-echo`

The client connector sends a `request` to the session server. The session server relays the message
to an actor on the actor server, and the actor server returns the reply of the same request to the
client through the session server.

| Item | Value |
|------|----|
| Server configuration | `SessionServer`, `ActorServer`, `Registry` if needed |
| Client count | Default 10,000 |
| Payload | 1 KiB representative, 4 KiB alongside |
| Mode | `request` |
| Measurement unit | Client-visible echo completion |
| Comparison purpose | Remote actor routing, server-to-server hop, and reply relay cost |

This scenario is a simplified Bingo-shaped structure. Session and actor must be on different server
processes. If they collapse into the same process, it can't be distinguished from the local
scenario, and is considered a failure.

### 10.3 `s2s-channel-to-spot-request-echo`

The channel server sends a request to a Spot on the remote Spot server and receives a reply. The
client only sends the benchmark trigger — the measured target is the server-to-server channel →
Spot request/reply.

| Item | Value |
|------|----|
| Server configuration | `ChannelServer`, `SpotServer`, `Registry` if needed |
| Payload | 4 KiB representative, 1 KiB alongside |
| Mode | `request` |
| Measurement unit | Server-to-server echo completion |
| Comparison purpose | Cost of a remote Spot request/reply from a channel |

### 10.4 `s2s-channel-to-spot-send-send-echo`

The channel server sends an echo request via send to the remote Spot server, and the Spot server
sends a send response to the channel server. In a language where the framework doesn't provide
request/reply correlation, `correlationId` is put into the payload and the benchmark harness
aggregates completion counts.

| Item | Value |
|------|----|
| Server configuration | `ChannelServer`, `SpotServer`, `Registry` if needed |
| Payload | 4 KiB representative, 1 KiB alongside |
| Mode | `send-send` |
| Measurement unit | Correlation completion count |
| Comparison purpose | Maximum throughput and loss rate when using bidirectional send without request/reply |

### 10.5 `s2s-spot-to-channel-request-echo`

The Spot server sends a request to the channel server and receives a reply. The trigger goes to the
Spot server, but the measured target is the Spot → channel request/reply.

| Item | Value |
|------|----|
| Server configuration | `SpotServer`, `ChannelServer`, `Registry` if needed |
| Payload | 4 KiB representative, 1 KiB alongside |
| Mode | `request` |
| Measurement unit | Server-to-server echo completion |
| Comparison purpose | Cost of an outbound channel request inside a Spot handler |

### 10.6 `s2s-spot-to-channel-send-send-echo`

The Spot server sends an echo request via send to the channel server, and the channel server sends
a send response to the Spot server.

| Item | Value |
|------|----|
| Server configuration | `SpotServer`, `ChannelServer`, `Registry` if needed |
| Payload | 4 KiB representative, 1 KiB alongside |
| Mode | `send-send` |
| Measurement unit | Correlation completion count |
| Comparison purpose | Maximum throughput and loss rate when using bidirectional send from a Spot to a channel |

### 10.7 `spot-async-request-echo`

The client sends a trigger request to the Spot server. The Spot handler sends a request to the
remote echo channel server and waits for the reply with a single `Async` terminator, then records
client-visible completion. The framework automatically returns the Spot turn during the wait and
resumes the continuation in the original dispatcher context once the reply arrives. This scenario
measures the cost of this automatic management path and queue progress.

| Item | Value |
|------|----|
| Server configuration | `SpotServer`, `RemoteEchoServer`, `Registry` if needed |
| Payload | 1 KiB representative, 4 KiB alongside |
| Mode | `async-request` |
| Spot placement | Distributes requests across `--spot-count` Spot RIDs |
| Measurement unit | Spot handler echo completion |
| Comparison purpose | Automatic turn management cost and queue progress for a remote-request wait inside a Spot handler |

### 10.8 `spot-await-contention`

Concentrates many requests on a single Spot RID. The handler sends a request to the remote echo
channel server and waits with a single `Async` terminator. Compared with the
`spot-async-request-echo` result — which spreads across multiple Spot RIDs under the same
conditions — this confirms how much automatic turn management reduces head-of-line blocking on a
single mailbox.

| Item | Value |
|------|----|
| Server configuration | `SpotServer`, `RemoteEchoServer`, `Registry` if needed |
| Payload | 1 KiB representative, 4 KiB alongside |
| Mode | `async-request` |
| Spot placement | Fixed at `--spot-count 1` |
| Measurement unit | Single Spot RID echo completion |
| Comparison purpose | Whether automatic turn management keeps queue progress when requests concentrate on a single Spot |

This scenario is fixed at `--spot-count 1` so load isn't spread across multiple Spot RIDs. Spreading
across multiple Spots would mix the queue-progress effect of automatic turn management with the
owner-distribution effect. When comparing, keep payload, in-flight, and the remote echo server the
same, and only change the Spot RID count.

### 10.9 `spot-no-await-echo`

The Spot handler doesn't call a remote request — it echoes the payload immediately. This baseline
is a reference for looking only at Spot dispatch and payload validation cost.

| Item | Value |
|------|----|
| Server configuration | `SpotServer` |
| Payload | 1 KiB representative, 4 KiB alongside |
| Mode | `no-await` |
| Measurement unit | Spot handler echo completion |
| Comparison purpose | Spot dispatch baseline for comparing against the automatic turn wait path |

A Spot execution scenario's handler doesn't reason about shared mutable state continuing across a
request boundary. Since other jobs can progress during automatic turn management, only flows that
don't carry shared state across a request boundary — like admission I/O or plain echo — are
measured.

### 10.10 `spot-worker-offload-echo`

The client sends a trigger request to the Spot server. The Spot handler offloads fixed-cost CPU work
to the framework worker pool via `runCpuWorker(...)` (per-language `RunCpuWorker`/`runCpuWorker`/
`run_cpu_worker`), waits with a single completion terminator, then records client-visible
completion. The framework automatically returns the Spot turn while waiting for the worker to
complete. A `RemoteEcho` server isn't needed — this axis measures the cost of offloading to the
local worker pool, not remote I/O. The worker task itself is a busy-wait or sleep fixed by
`--worker-task-millis`, and a per-language implementation doesn't put arbitrary CPU work in it.

| Item | Value |
|------|----|
| Server configuration | `SpotServer` |
| Payload | 1 KiB representative, 4 KiB alongside |
| Mode | `worker-offload` |
| Worker configuration | `--worker-task-millis`, `--worker-pool-size` |
| Measurement unit | Spot handler echo completion |
| Comparison purpose | Automatic turn management cost for a worker-pool completion wait, and worker→Spot continuation-resume cost |
| Failure classification | Records `WorkerQueueFull`, `WorkerTimeout` separately in `errors.byKind` |

This scenario measures the worker-offload wait path verified by TD-C3/TD-C4 of
`config-8-execution-turn.en.md` under the same conditions. The `runCpuWorker(...)` measured here is
a Spot-dedicated worker thread pool offload within the same process. It doesn't mean a separate
public contract for distributing work across multiple processes.

### 10.11 `actor-no-bind-request-echo`

A session-less `ActorCaller` server sends a request to an `ActorRef` obtained in advance via the
actor client's `RequestToActor` call, and receives a reply. The client only sends the benchmark
trigger to `ActorCaller` — the measured target is the server-to-server actor client no-bind
request/reply. `RequestToActor`'s await completion means the handler reply arrived.

| Item | Value |
|------|----|
| Server configuration | `ActorCaller`, `Actor`, location store (Redis) |
| Payload | 4 KiB representative, 1 KiB alongside |
| Mode | `request` |
| Measurement unit | Server-to-server echo completion |
| Comparison purpose | No-bind delivery cost when requesting via `ActorRef` without a session bind |
| Failure classification | Records `ActorRouteNotFound`, `ActorLocationStale`, `RouteNotConnected` separately in `errors.byKind` |

This scenario always targets an unbound actor. The bind-state matrix (TA-A1~A4) of
`config-9-to-actor-messaging.en.md` is aimed at functional verification, so it isn't recreated in
perf.

### 10.12 `actor-no-bind-send-send-echo`

The `ActorCaller` server sends an echo request via `SendToActor` to an `ActorRef`, and the actor
handler sends a send response back to the same `ActorCaller`. Since `SendToActor`'s own await
completion (successful resolve + local mailbox handoff) and the actual echo round trip (correlation
completion) happen at different moments, they're recorded separately.

| Item | Value |
|------|----|
| Server configuration | `ActorCaller`, `Actor`, location store (Redis) |
| Payload | 4 KiB representative, 1 KiB alongside |
| Mode | `send-send` |
| Measurement unit | Correlation completion count (local handoff latency recorded separately) |
| Comparison purpose | Maximum throughput and local-handoff cost when accessing an actor via bidirectional send without a session bind |
| Failure classification | Records `ActorRouteNotFound`, `ActorLocationStale`, `RouteNotConnected` separately in `errors.byKind` |

### 10.13 `pubsub-fanout-echo`

The `Publisher` server continuously publishes events to one fixed topic, and `--subscriber-count`
`Subscriber` servers receive the same events. The client only sends the benchmark trigger to
`Publisher` — the measured target is publisher → multiple-subscriber fanout. Since
`Publish(...).Async()` doesn't guarantee remote receipt, the completion criterion isn't
client-visible echo completion — it's defined as per-subscriber delivery in the table below.

| Item | Value |
|------|----|
| Server configuration | `Publisher`, `Subscriber` × `--subscriber-count`, location store (Redis) |
| Payload | 1 KiB representative, 4 KiB alongside |
| Mode | `publish` |
| Measurement unit | Per-subscriber received event count, `fanout.deliveryRatio`, delivery latency |
| Comparison purpose | How publish throughput and per-subscriber delivery latency change with fanout width (subscriber count) |
| Failure classification | A missed receipt isn't an error — it's observed as a drop in `fanout.deliveryRatio`. Only transport errors are recorded in `errors.byKind` |

This scenario doesn't reproduce topic filtering (covered by PS-A2 of `config-3-pubsub.en.md`), a
late subscriber joining, or dynamic events like publisher/subscriber restart. That kind of
functional verification is e2e Config 3's job — perf only measures fanout throughput in a normal
state where subscription is already complete. It only moves to the measured phase after the warmup
phase waits until every subscriber has received at least one event.

## 11. Baseline Scenarios

With only integration scenarios, results are hard to interpret. Each language keeps the baselines
below alongside them where possible. Baselines aren't a mandatory release-gate item, but are run
first during bottleneck analysis.

| Baseline | Purpose |
|----------|------|
| `connector-echo-only` | Measures only connector dispatch and codec cost, with no session/actor |
| `session-echo-only` | Measures the cost when the session handler echoes directly, with no actor |
| `channel-echo-only` | Measures channel request/send cost, with no Spot |
| `spot-local-echo` | Measures Spot dispatch cost, with no remote hop |
| `spot-no-await-echo` | Measures Spot handler dispatch cost, with no remote request |

For a language that doesn't yet have a baseline, an integration-scenario result is recorded as
"bottleneck location undetermined" when interpreted.

## 12. Message Contract

Per-language implementations use message contracts with the same meaning. Names follow per-language
casing, but field meaning doesn't change.

| Message | Fields | Meaning |
|--------|------|------|
| `PerfEchoRequest` | `runId`, `clientId`, `sequence`, `correlationId`, `sentTicks`, `payload` | An echo request |
| `PerfEchoReply` | `runId`, `clientId`, `sequence`, `correlationId`, `receivedTicks`, `payload` | An echo reply |
| `PerfTriggerRequest` | `runId`, `batchSize`, `mode`, `payloadSize`, `payload` | Starts a server-to-server echo batch |
| `PerfTriggerReply` | `accepted`, `completed`, `failed`, `message` | Trigger processing result |
| `PerfPublishEvent` | `runId`, `sequence`, `topic`, `sentTicks`, `payload` | A fanout event the publisher publishes |
| `PerfMetricsSnapshot` | The §14 metric fields below | Result of a client/server metric query |

The receiving side confirms `payload`'s size and part of its byte pattern. In an echo test, the
payload must be returned as-is. Recreating, compressing, or partially returning the payload changes
the measurement meaning, so it's considered a failure.

Unlike `PerfEchoRequest`, `PerfPublishEvent` has no `clientId`/`correlationId`. A publish doesn't
target a specific receiver, so there's no clientId concept, and since there's no reply, there's no
correlation either. Instead, `sequence` is the only key that verifies continuous per-subscriber
receipt. `topic` always uses the same fixed value in `pubsub-fanout-echo`. Per-topic branching isn't
part of the perf scope.

## 13. Fairness Between The Request Style And The Send/Send Style

`request` and `send-send` have different meanings, so their numbers must not be compared directly.

- `request` has the caller wait for the reply. The framework's request timeout and cancellation
  policy apply.
- `send-send` builds an echo from two one-way messages. The benchmark harness counts completion by
  `correlationId` and manages timeout separately.
- Both styles have an overall in-flight cap. If `send-send` pushes unbounded, it can't be compared
  with request.
- The `send-send` result must record `sent`, `completed`, `expired`, `duplicateReply`, and
  `unknownCorrelation`.

`send-send` latency is calculated from when `correlationId` is registered to when the reply arrives.
A message with no reply isn't included in the latency percentile calculation — it goes into the
timeout/error count.

## 14. Metrics

Every language uses the same metric keys in the result file.

| Metric | Unit | Description |
|--------|------|------|
| `connections.requested` | count | Total requested connector count |
| `connections.connected` | count | Connector count prepared before the measured phase |
| `connections.failed` | count | Connection or authentication failure count |
| `messages.sent` | count | Requests or sends made during the measured phase |
| `messages.completed` | count | Echo completion count |
| `messages.failed` | count | Failure response or handler error count |
| `messages.timeout` | count | Request timeout or send/send correlation timeout |
| `throughput.kops` | ops/sec / 1000 | KOPS based on echo completion |
| `throughput.messagesPerSec` | msg/sec | Throughput based on actually sent messages |
| `throughput.megabytesPerSec` | MiB/sec | Throughput based on payload bytes |
| `latency.meanMs` | ms | Mean latency |
| `latency.p50Ms` | ms | p50 latency |
| `latency.p95Ms` | ms | p95 latency |
| `latency.p99Ms` | ms | p99 latency |
| `latency.maxMs` | ms | Maximum latency |
| `process.cpuPercent` | percent | Process CPU usage |
| `process.rssMb` | MiB | Resident memory |
| `process.allocatedMb` | MiB | Allocated bytes, if the runtime can provide it |
| `gc.gen0`, `gc.gen1`, `gc.gen2` | count | GC counts. `null` for a language without GC |
| `errors.byKind` | object | Error classification such as timeout, decode, route, connection, handler. The `actor-no-bind-*` scenarios also record `ActorRouteNotFound`, `ActorLocationStale`, `RouteNotConnected` as separate keys |
| `actor.localHandoff.latency.p95Ms` | ms | p95 time to `SendToActor` await completion (local mailbox handoff) |
| `actor.localHandoff.latency.p99Ms` | ms | p99 time to `SendToActor` await completion (local mailbox handoff) |
| `spot.mailboxDepth.max` | count | Maximum Spot mailbox depth observed during measurement |
| `spot.mailboxDepth.mean` | count | Mean Spot mailbox depth observed during measurement |
| `spot.suspendedTurns` | count | Number of Spot turns the framework automatically returned while waiting for async completion |
| `spot.resumedTurns` | count | Number of Spot turns resumed in the original dispatcher context after completion |
| `spot.resumeLatency.p95Ms` | ms | p95 time from reply-receivable moment to coroutine resume |
| `spot.resumeLatency.p99Ms` | ms | p99 time from reply-receivable moment to coroutine resume |
| `spot.remoteRequestRtt.p95Ms` | ms | p95 RTT of the remote request the Spot handler called |
| `spot.remoteRequestRtt.p99Ms` | ms | p99 RTT of the remote request the Spot handler called |
| `worker.pool.queueDepth.max` | count | Maximum worker pool queue depth observed during measurement |
| `worker.pool.queueDepth.mean` | count | Mean worker pool queue depth observed during measurement |
| `worker.pool.queueWaitLatency.p95Ms` | ms | p95 time from `runCpuWorker(...)` submission to the worker thread picking it up |
| `worker.pool.queueWaitLatency.p99Ms` | ms | p99 time from `runCpuWorker(...)` submission to the worker thread picking it up |
| `worker.taskLatency.p95Ms` | ms | p95 duration of the task execution itself (`--worker-task-millis`) on the worker thread |
| `worker.taskLatency.p99Ms` | ms | p99 duration of the task execution itself (`--worker-task-millis`) on the worker thread |
| `worker.resumeLatency.p95Ms` | ms | p95 time from worker task completion to original Spot mailbox continuation resume |
| `worker.resumeLatency.p99Ms` | ms | p99 time from worker task completion to original Spot mailbox continuation resume |
| `messages.published` | count | Events the publisher published during the measured phase |
| `fanout.subscriberCount` | count | Number of subscriber processes participating in this run |
| `fanout.deliveryRatio` | ratio(0~1) | The minimum, across subscribers, of (unique sequences received / `messages.published`) |
| `fanout.deliveryLatency.p95Ms` | ms | p95 time from publish moment to subscriber receipt |
| `fanout.deliveryLatency.p99Ms` | ms | p99 time from publish moment to subscriber receipt |

KOPS is calculated as `messages.completed / measuredSeconds / 1000`. Since a request/reply echo can
internally produce multiple messages, KOPS and `messagesPerSec` are recorded separately.
Spot-related metrics, `actor.*` metrics, `fanout.*` metrics, and `worker.*` metrics are left as
`null` if the scenario doesn't support them. Arbitrarily filling a value with `0` would make it
indistinguishable from an actual 0. `pubsub-fanout-echo`'s `messages.completed` is always `null`
since there's no client-visible echo — success is instead judged by `fanout.deliveryRatio` and
`messages.published`.

To aggregate latency percentiles across multiple client processes, histogram buckets are needed.
Every client and server snapshot records a histogram in the format below together.

```json
{
  "latencyMs": {
    "unit": "ms",
    "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
    "counts": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "overflow": 0
  }
}
```

`bounds` are the bucket upper bounds, and `counts` is the bucket count at the same index. The
`counts` length must equal the `bounds` length. A value larger than the upper bound goes into
`overflow`. When merging results from multiple client processes, only histograms using the same
`bounds` are aggregated. If the bounds differ, the runner must fail.

## 15. Result File Format

Each run leaves results under `perf-results/<run-id>/`.

```text
perf-results/<run-id>/
|-- config.json
|-- result.json
|-- summary.txt
|-- client-<index>.json
|-- server-<role>.json
`-- logs/
    |-- client-<index>.log
    |-- server-<role>.log
    `-- message-flow-<role>.log
```

`result.json` is the machine-readable result. Its minimum fields are as follows.

```json
{
  "runId": "20260626-123000",
  "language": "dotnet",
  "scenario": "cs-local-session-actor-echo",
  "mode": "request",
  "payloadSize": 1024,
  "connections": 10000,
  "clientCount": 1,
  "durationSeconds": 30,
  "warmupSeconds": 5,
  "measuredSeconds": 30.002,
  "metrics": {
    "connections.requested": 10000,
    "connections.connected": 10000,
    "messages.sent": 120000,
    "messages.completed": 120000,
    "throughput.kops": 0.0,
    "latency.p99Ms": 0.0
  },
  "histograms": {
    "latencyMs": {
      "unit": "ms",
      "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
      "counts": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
      "overflow": 0
    }
  },
  "clients": [
    {
      "clientIndex": 0,
      "clientIdStart": 0,
      "clientIdEndExclusive": 10000,
      "metricsFile": "client-0.json"
    }
  ],
  "servers": {
    "sessionActorLocal": "server-sessionActorLocal.json"
  }
}
```

`client-<index>.json` is the raw result from one client process. Its minimum fields are as follows.

```json
{
  "runId": "20260626-123000",
  "clientIndex": 0,
  "clientCount": 4,
  "clientIdStart": 0,
  "clientIdEndExclusive": 2500,
  "warmupConnections": 2500,
  "metrics": {
    "connections.requested": 2500,
    "connections.connected": 2500,
    "messages.sent": 30000,
    "messages.completed": 30000
  },
  "histograms": {
    "latencyMs": {
      "unit": "ms",
      "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
      "counts": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
      "overflow": 0
    }
  }
}
```

`result.json` is the count, byte, and error metrics of `client-<index>.json` summed, with the
histogram merged and the percentile recalculated. A percentile value like `latency.p95Ms` must not
be calculated as an average of per-process percentiles.

`summary.txt` is the human-readable result. This file shows configuration, throughput, latency
percentiles, failure counts, and client/server CPU and memory on one screen.

## 16. Server Metrics Endpoint

Each server role provides a metrics endpoint dedicated to performance testing. The transport
defaults to HTTP. A language where attaching an HTTP server is difficult can use a separate admin
channel, but the runner must be able to call `reset` and `snapshot` with the same meaning.

| Endpoint | Meaning |
|----------|------|
| `POST /perf/reset` | Resets the server metrics |
| `GET /perf/stats` | Returns the current server metrics snapshot |
| `GET /perf/ready` | Returns whether the server is ready to receive benchmark requests |

The `POST /perf/reset` request and response follow the format below.

```json
{
  "runId": "20260626-123000",
  "scenario": "cs-local-session-actor-echo",
  "mode": "request",
  "payloadSize": 1024,
  "resetSeq": 1,
  "resetAtUnixMs": 1782466200000
}
```

```json
{
  "ok": true,
  "role": "sessionActorLocal",
  "runId": "20260626-123000",
  "resetSeq": 1,
  "resetAtUnixMs": 1782466200000
}
```

The `GET /perf/ready` response follows the format below.

```json
{
  "ready": true,
  "role": "sessionActorLocal",
  "runId": "20260626-123000",
  "endpoints": {
    "appEndpoint": "tcp://127.0.0.1:21001",
    "metricsUrl": "http://127.0.0.1:31001"
  },
  "message": ""
}
```

The `GET /perf/stats` response follows the format below.

```json
{
  "role": "sessionActorLocal",
  "runId": "20260626-123000",
  "scenario": "cs-local-session-actor-echo",
  "mode": "request",
  "window": {
    "startedAtUnixMs": 1782466205000,
    "endedAtUnixMs": 1782466235002,
    "measuredSeconds": 30.002
  },
  "metrics": {
    "messages.completed": 120000,
    "process.cpuPercent": 240.5
  },
  "histograms": {
    "latencyMs": {
      "unit": "ms",
      "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
      "counts": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
      "overflow": 0
    }
  }
}
```

The metrics endpoint must not sit on the measured path. If the echo handler calls the HTTP metrics
endpoint for every message, or creates significant lock contention, measurement is distorted. On
the hot path, only an atomic counter and a low-cost latency recorder are used.

## 17. Per-Language Standard Location

Per-language perf code defaults to living under `framework/languages/<lang>/perf/`. Since Kotlin
shares a build root with the Java runtime, it follows this chapter's Kotlin location.

### 17.1 .NET

```text
framework/languages/dotnet/perf/
|-- ZLink.Framework.Perf.Shared/
|-- ZLink.Framework.Perf.Client/
|-- ZLink.Framework.Perf.SessionActorLocalServer/
|-- ZLink.Framework.Perf.SessionServer/
|-- ZLink.Framework.Perf.ActorServer/
|-- ZLink.Framework.Perf.ChannelServer/
|-- ZLink.Framework.Perf.SpotServer/
|-- ZLink.Framework.Perf.RemoteEchoServer/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

The .NET implementation defaults to a Release build, and server metrics include
`GC.CollectionCount`, `GC.GetTotalAllocatedBytes`, process RSS, and process CPU.

### 17.2 Java

```text
framework/languages/java/perf/
|-- shared/
|-- client/
|-- session-actor-local-server/
|-- session-server/
|-- actor-server/
|-- channel-server/
|-- spot-server/
|-- remote-echo-server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

The Java implementation provides a Gradle standalone runner. Server metrics include JVM GC count,
heap/non-heap usage, and process CPU.

### 17.3 Kotlin

Since Kotlin shares a build root with the Java framework, its standard location is fixed at
`framework/languages/java/perf/kotlin/`. It must use the same scenario names and result schema as
Java.

```text
framework/languages/java/perf/kotlin/
|-- shared/
|-- client/
|-- session-actor-local-server/
|-- session-server/
|-- actor-server/
|-- channel-server/
|-- spot-server/
|-- remote-echo-server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

### 17.4 Node.js

```text
framework/languages/node/perf/
|-- shared/
|-- client/
|-- session-actor-local-server/
|-- session-server/
|-- actor-server/
|-- channel-server/
|-- spot-server/
|-- remote-echo-server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

The Node.js implementation separates TypeScript source and build output. Metrics include event loop
delay, RSS, heap used, and process CPU.

### 17.5 C++

```text
framework/languages/cpp/perf/
|-- shared/
|-- client/
|-- session_actor_local_server/
|-- session_server/
|-- actor_server/
|-- channel_server/
|-- spot_server/
|-- remote_echo_server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

The C++ implementation uses release build artifacts. If the core runtime or bindings runtime path
is older than the source, the runner must fail. A performance figure must not be interpreted from a
stale runtime.

## 18. Implementation Order

Every language implements all the way to the final scenario in one pass, but the work order follows
the list below. The order is meant to reduce debugging cost — it doesn't mean releasing features in
separate stages.

1. Build the common message contract and result schema.
2. Build the metrics collector and `POST /perf/reset`, `GET /perf/stats`, `GET /perf/ready`.
3. Build the client runner's connect, warmup, measured, and report phases.
4. Confirm the runner and metrics are correct with the `connector-echo-only` and `session-echo-only`
   baselines.
5. Implement `cs-local-session-actor-echo`.
6. Implement `cs-remote-session-actor-echo`.
7. Implement the `channel-echo-only` and `spot-local-echo` baselines.
8. Implement channel → Spot request and channel → Spot send/send.
9. Implement Spot → channel request and Spot → channel send/send.
10. Implement `spot-no-await-echo` and `spot-async-request-echo`.
11. Implement `spot-await-contention` for single-Spot-RID concentrated load.
12. Implement `spot-worker-offload-echo`. Since it's a path waiting for `runCpuWorker(...)`
    completion with only the `SpotServer` and no `RemoteEcho` server, it can proceed independently
    of steps 10–11.
13. Every framework language that provides an actor client implements
    `actor-no-bind-request-echo` and `actor-no-bind-send-send-echo`.
14. Implement `pubsub-fanout-echo`. First build the `Publisher`/`Subscriber` roles and the
    `--subscriber-count` split, then align the warmup's "confirm every subscriber's first receipt"
    barrier.
15. Have `run_perf.sh` run every standard scenario and collect results under one directory.
16. Have a Codex agent review the documentation, scenario names, result schema, and public API
    usage.
17. Repeat fixes and review until no issues remain, and consider it complete once the last review
    is `LOOP CLEAN`.

## 19. Regression And Comparison Criteria

Since performance figures are heavily affected by the environment, they aren't failed against a
fixed threshold from the start. Instead, the following criteria are recorded with the result.

- git commit hash
- core/bindings/framework build mode and runtime path
- CPU model, core count, memory
- OS, kernel, container status
- `ulimit -n`
- Client process count and per-client connector count
- Payload size (`1024` or `4096`), duration, warmup, in-flight
- Per-server-role process id and endpoint

Regression judgment compares against a baseline from the same machine, same configuration, and same
scenario. When put in a release gate, look at throughput drop rate, p99 increase rate, and
timeout/error increase rate together. Don't fail on a single metric alone.

## 20. Operating System And Execution Environment

A 10,000-connector test is sensitive to OS configuration. Before running, the runner checks the
following items and prints a clear error if any are insufficient.

- File descriptor limit
- Ephemeral port range
- Listen backlog
- TCP TIME_WAIT reuse policy
- Whether the client runner's CPU is saturated
- Whether server and client are on the same host or different hosts

Running client and server together on the same host mixes loopback performance with CPU contention.
This mode is useful for development and regression tracking, but isn't generalized as a network
performance figure. To see the real limit, distribute the client runner across a different host or
multiple hosts.

## 21. Prohibited

- Don't build the measured path using framework-internal runtime APIs, private APIs, or reflection
  bypasses.
- Don't copy sample or e2e code to build a benchmark server heavy with domain rules.
- Don't log on every message inside the measured phase. Only leave errors and aggregate metrics.
- Don't judge readiness only with a fixed sleep.
- Don't compare the request style and the send/send style without an in-flight limit.
- Don't compare an automatic-turn scenario and a baseline under different remote echo server,
  payload, or in-flight conditions.
- Don't change worker-offload conditions with a different `--worker-task-millis` or
  `--worker-pool-size` on each run.
- Don't put business logic that carries shared mutable state across a request boundary into an
  automatic-turn performance scenario.
- Don't skip payload validation. If a broken echo looks like a fast success, the result is
  meaningless.
- Don't mix failed messages into the latency percentile. Record failures as a separate error
  metric.
- Don't record a stale build artifact or debug build result as a release performance figure.

## 22. Completion Criteria

A per-language perf implementation is considered complete once it satisfies the conditions below.

- It supports every standard scenario name.
- Each scenario follows the `1 KiB` and `4 KiB` payloads and the default connection count.
- Client/server metrics are saved in the common result schema.
- `request` and `send-send` run under the same in-flight criterion.
- The Spot automatic-turn scenario and its baseline run under the same remote echo server, payload,
  and in-flight criteria.
- The Spot worker-offload scenario runs under a fixed `--worker-task-millis`, `--worker-pool-size`,
  payload, and in-flight criteria.
- For `pubsub-fanout-echo`, every `Subscriber` process's metrics are collected as individual files,
  and `fanout.deliveryRatio` is recorded in the result.
- The server metrics endpoint is reset after warmup and snapshotted after the measured phase.
- If failures and timeouts aren't zero, the summary shows a per-cause count.
- `run_perf.sh` can run every standard scenario.
- Repeated Codex agent review confirms no issues remain.

## 23. How To Decide The Application HWM As A Production Workload

This section is the common measurement specification for deciding the host-wide HWM of application
payload received but not yet completed by a handler, as a production workload. The HWM's
configuration mode and Auto calculation contract are based on
[Framework API "2.1 Keeping Received Payload From Growing Memory Indefinitely"](../spec/06-framework-api.en.md#21-keeping-received-payload-from-growing-memory-indefinitely).
This section only defines the execution method for choosing a per-application positive HWM and
verifying an Auto HWM profile.

### 23.1 Conditions To Fix First

Use the same CPU quota, process memory limit, connection count, dispatch concurrency, and
runtime/GC options as production. Fix the following characteristics of the job workload together.

- The ratio of request to one-way jobs
- The application payload size distribution
- The ratio of CPU-bound to I/O-bound handlers
- The reply size and nested-request ratio a handler produces
- Burst ingress rate and duration

HWM doesn't limit CPU usage. If the target is `50%` of the allocated CPU quota, adjust dispatch
concurrency or a separate rate limit first. Only use a run where normalized process CPU stays in the
`45–55%` range while backlog persists as the target capacity measurement.

### 23.2 Throughput Measurement

Run a `60`-second measured phase, `5` times, after at least a `30`-second warm-up. If the
coefficient of variation of throughput across the five runs exceeds `5%`, lengthen the measured
phase. During measurement, jobs waiting on dispatch must keep existing continuously. A run where
the handler idled due to insufficient ingress is excluded from the processing-capacity result.

Each run's throughput is calculated as follows.

```text
runDrainBytesPerSecond =
    terminalPayloadBytes / measuredWallClockSeconds

measuredSustainableDrainBytesPerSecond =
    minimum(runDrainBytesPerSecond across five valid runs)
```

`terminalPayloadBytes` includes, exactly once, the application payload bytes of the original job
that reached the handler terminal during the measured phase. It doesn't include reply bytes,
ingress bytes, or instantaneous peak throughput.

### 23.3 Calculating The HWM Candidate

Decide the maximum queue delay operations will tolerate, and calculate the throughput-based
candidate.

```text
queueDelayCandidateBytes =
    measuredSustainableDrainBytesPerSecond
    * maximumQueueDelaySeconds

candidateApplicationHwmBytes =
    measuredPeakActiveHandlerPayloadBytes
    + queueDelayCandidateBytes
```

`measuredPeakActiveHandlerPayloadBytes` is the maximum, during the measured phase, of the sum of
payload bytes held by handler contexts currently executing. Since the Application HWM also includes
payload of an executing handler, this value is added to the queue-delay candidate.

`autoProfileHwmBytes` is the allocated application memory multiplied by the chosen profile ratio.
`COMPACT` uses `2%`, `LOW_LATENCY` uses `5%`, `BALANCED` uses `10%`, and `THROUGHPUT` uses `20%`.
When comparing the four Auto profiles, run each with the same workload. Choose the largest profile
that satisfies the memory and queue-delay conditions, and use `BALANCED` if no separate choice is
made.

To fix a per-application production value, set `candidateApplicationHwmBytes` as a positive
`ApplicationHwmBytes` and measure again. Fill the backlog up to the HWM and confirm all of the
following conditions.

- Peak process memory doesn't exceed the process limit.
- Queue-delay p99 and maximum satisfy the application's target.
- Throughput and CPU stay within the range allowed by the capacity test with HWM off.
- Pause and source backpressure occur without message drops.
- One message larger than the HWM is received when there's no application job currently being
  processed.

If the conditions aren't satisfied, lower the HWM and repeat the same test. Use the largest value
that satisfies the conditions as the production value.

### 23.4 Values That Must Be Recorded In The Result

- CPU quota/core count, process memory limit, runtime/GC configuration, and Framework version
- Connection count, dispatch concurrency, request/one-way ratio, and job size distribution
- Per-run terminal job count, terminal payload bytes, wall-clock throughput, and process CPU
- Peak RSS, peak pending application payload bytes, and active handler payload bytes
- Queue-delay p50/p95/p99/max, pause count/duration, and backpressure count
- The chosen Auto HWM profile and the memory value used to calculate it
- Auto profile HWM, queue-delay candidate, active handler payload, production HWM, and the reasoning
  for the choice
