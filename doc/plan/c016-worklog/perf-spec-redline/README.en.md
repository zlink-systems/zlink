# Framework Performance Test Common Specification

[Document list][docs] · [Common specification][common] · [Scenario E2E][e2e]

This specification defines execution conditions and result formats for comparing throughput,
latency and resource use through the same public calls in .NET, C++, Java, Kotlin and Node.js.
It owns standard scenario names, the payload matrix, CLI and result formats. Language plans
supplement only implementation tools and runtime metadata.

The contracts below own runtime behavior. Perf measures their public calls, callbacks and status;
it adds no connection selection, replay, turn, completion or error contract.

| Measurement boundary | Owning contract |
|---|---|
| Turns and ordinary/Yield terminals | [Submit and completion §2][submit], [Handler turns and execution gates §2–4][turn] |
| One-way admission, request completion and deadlines | [Submit and completion §4–9][submit] |
| Global ActorId messaging | [Actor model §5][actor] |
| Global SpotId and preparation | [Spot address messaging §2–3][spot-address], [MeshNode §4][mesh] |
| STREAM callbacks, bind and relay | [STREAM session][session], [Session and Actor binding §5, §12][binding] |
| ChannelName and topology | [Channel messaging][channel], [ClientServer Channel §5][clientserver] |
| Classic fanout | [Framework API fanout contract][api], [Config 3 public observations][fanout] |
| Host capacity and observation limits | [Application job queue][queue], [Runtime monitoring §4][monitor], [Runtime metrics §3, §11][metrics] |
| Public errors | [Framework error model][errors] |
| Physical admission and completion drain | [Core socket][core-socket], [Binding async execution model §4][binding-async] |

## 1. Goals

Performance results must answer these questions.

- How do client and server throughput, latency and CPU use change with many STREAM connections?
- How much does cost differ between a session and Actor on the same local object node and separate processes?
- How do completion rate, throughput and tail latency differ for Channel↔Spot request/reply and send/send?
- When selecting the ordinary terminal that retains a [Spot turn][g-turn] or Yield that releases it,
  how do Spot count, remote call completion and progress of other callbacks relate ([owning contract][turn])?
- How much do CPU worker execution and submission/result-delivery intervals contribute to completion latency?
- Can differences between 1 KiB and 4 KiB payloads be related to client, server, codec or transport bottleneck candidates?

## 2. Scope

Standard scenarios use the names in §10 and run with `1024` and `4096` logical payload bytes each.
An independent execution of one scenario, payload, terminal and placement combination is a
**measurement cell**. Throughput from different cells is not averaged into one result.

| Family | Question covered | Representative payload |
|---|---|---:|
| CS | Connector → session → local/remote Actor echo | 1024 |
| S2S | Channel → Spot and Spot → Channel request or send/send | 4096 |
| Spot local/worker | Local public Spot dispatch and CPU offload | 1024 |
| AC | Session-less ActorCaller → ActorId direct messaging | 4096 |
| PS | Classic fanout publication and subscriber delivery | 1024 |

A [Spot][g-spot] is a logical object with an address and state; these cells use prepared User Spots.
Their [Spot IDs][g-spot-id] are recorded in `spotIds`. Object roles and Store requirements follow
[MeshNode][mesh]; cells needing a Store use the dedicated Docker Redis in §20.
ActorRef is used only to inspect create results and bind sessions. Measured messaging targets
follow the [Actor model][actor] and [Spot address contract][spot-address].

The `session-echo-only` baseline, both RouteMesh/ClientServer request cells of `channel-echo-only`,
and the `spot-no-await-echo` local reference in §11 are required reference measurements.
Section 19 governs release performance thresholds. Fake backend and micro benchmark figures
are not counted toward common perf completion.

### 2.1 Follow-up candidates

| Candidate | Separate question | Owning contract |
|---|---|---|
| Instance Spot cold/hot | First call to a new ID versus calls to a prepared ID | [Spot address messaging][spot-address] |
| Logical Multicast | Target-node selection and local Spot delivery cost | [Spot messaging §4][spot] |
| Relocation under load | Completion rate, p99, interruption and resumption during movement | [Host relocation][relocation] |
| .NET/C++ HTTP | HTTP JSON binding, DI and handler round-trip cost | [.NET HTTP][http-dotnet], [C++ HTTP][http-cpp] |

These candidates are excluded from standard cells, accepted CLI scenarios and completion counts.

## 3. The Relationship Between Performance Tests And Other Tests

| Test kind | Purpose | Execution boundary |
|---|---|---|
| unit/contract | Correctness of public behavior and invariants | Smallest contract-appropriate configuration |
| sample | Application usage flow | Real client/server |
| E2E | Deployment combinations, failure and recovery | Real multi-process |
| perf | Completion rate, throughput, latency and resource use | Real role processes and public APIs |

- **Perf servers are independent echo applications.** Mixing sample business rules and E2E fault
  injection into the measured path prevents comparison of the same public-call cost.
- **Functional correctness is checked against the owner's contract/E2E observations.** A performance
  figure does not prove the correctness of internal behavior.

## 4. Common Execution Model

Every runner executes a cell in this order. Standard runs are duration based; short count-based
runs are separate smoke results.

| Phase | Participant and result | Throughput inclusion |
|---|---|---|
| build/preflight | Script checks release artifacts, configuration, OS, ports and output paths | Excluded |
| server start | Starts role processes and required Store, then checks §16 readiness | Excluded |
| connect/setup | CS clients connect and finish create/bind; server-driven callers prepare objects and consumers | Separate setup figures |
| warmup | Executes the same calls on every connector or logical stream in the cell | Excluded |
| reset | Stops submission, terminates remaining work and confirms the same resetSeq from all participants | Excluded |
| measured | Owners past the start barrier run load for a monotonic duration | Included |
| settle | Observes remaining results for a bounded time without new measured operations | Separate |
| report | Collects owner originals and histograms into a cell result | Excluded |
| cleanup | Stops clients/servers owned by this cell and cleans up run-owned resources | Excluded |

### 4.1 Windows and reset

Operations started in the measured interval form the **measured cohort**.
Each aggregation owner's interval is `[startTicks, endTicks)` on its monotonic clock, with start
and actual elapsed time recorded in its original file. HTTP response times are not assumed equal
across processes.

- **Stop warmup submissions and wait for all their terminals before reset.** Old callbacks must not
  contaminate new counters. Preparatory durable lifecycle operations also finish before this point;
  their replay and deadline semantics belong to [Actor §8.1][actor].
- **Open the start barrier only after every role and CS client acknowledges the same resetSeq.**
  Reset calls are not atomic across processes. Record application-counter reset separately from
  public `ResetCapacityMetrics` results ([metric epoch contract][metrics]).
- **Start no new operation after the measured interval.** Settle observes this cohort's outcomes.
  Preserve the first result in §13; do not change it with a new deadline or retry after timeout.
- **Preserve failed originals if warmup drain or settle exceeds its bound.** Recreating connections
  or adding arbitrary sleeps would hide unfinished work and invalidate comparisons.

`messages.completed` counts cohort successes whose validation also ends inside the window.
Successes finishing after the window but within settle use `messages.settleCompleted` and a
separate histogram. The throughput denominator excludes settle time. Operations unresolved at
settle end use `messages.unresolved`: this is the harness observation boundary, not a
reclassification of a Framework timeout ([completion contract][submit]).

### 4.2 Server-driven load

An independent workload flow in which a server process repeats public calls is a **logical stream**.
It is a different unit from a CS physical connector and applies to S2S, AC, PS and Spot local/worker.
One standalone client signals phase start through `applicationTriggerUrl`. This HTTP request and
response are neither measured operations nor KOPS. `/perf/*` does not start load.

Role config owns logical stream count, per-stream in-flight, duration and deadlines.
A trigger carries only `runId`, `cellId`, `resetSeq` and phase; it cannot alter those settings.
A phase starts once; duplicate triggers return the same start acknowledgement.

For cells measuring an outbound call inside a Spot handler, an application driver in the same
process invokes the handler through public Spot request/send. It reserves the stream's in-flight
slot before the call and holds it through the final echo outcome. Local driver calls are not
additional KOPS. The primary latency in §10.5 starts immediately before the handler's remote call
and ends at completion; the whole driver interval is recorded separately as `driver.latency.*`.
Each interval includes source-admission waiting from its public-call start.
A local driver request arriving after measured end starts no outbound call, returns
`PerfDriveReply.started=false`, and increments `driver.notStarted`.

## 5. Common CLI

Shell option names and consumers are defined below. Explicit options inapplicable to a cell, or
incompatible mode, terminal or topology values, are preflight errors. Defaults are passed only
to applicable consumers.

| Option | Default and range | Consumer and meaning |
|---|---|---|
| `--scenario` | Required for `run_single.sh`; all for `run_perf.sh` | Script selects an executable name from §8.4 or §11 |
| `--connections` | 10000, positive int32 | CS connection pool only: total physical connectors |
| `--logical-streams` | 10000, positive int32 | Server-driven source workload loop: stream count |
| `--client-count` | 1, positive int32 | Script and CS partition plan: client process count; 1 for server-driven cells |
| `--client-index` | 0, `0 <= index < count` | Script assigns the partition index to a child CS client; not a top-level input |
| `--duration-seconds` | 30, finite > 0 | Aggregation owner's measured window |
| `--warmup-seconds` | 5, finite > 0 | The same owner's warmup loop |
| `--payload-size` | Scenario representative; 1024 or 4096 | `run_single.sh` and payload factory |
| `--payload-sizes` | `1024,4096` | `run_perf.sh` matrix expansion; mutually exclusive with the single-value option |
| `--inflight` | 1, positive int32 | Logical-operation cap per CS connector or server-driven stream; publish-admission cap for PS |
| `--connect-concurrency` | 256, positive int32 | CS pool's concurrent connect/setup count; absent for server-driven cells |
| `--spot-count` | 16, positive int32 | Spot Object Server preparation and stream→Spot mapping; §10.5 standard matrix uses 1/16 |
| `--subscriber-count` | 8, positive int32 | PS script's independent Subscriber process count |
| `--worker-task-millis` | 5, positive int32 | CPU-duration target in the worker callback (§10.8) |
| `--worker-pool-size` | 8, positive int32 | Worker host's public MaxThreads setting |
| `--mode` | Fixed by scenario | Dispatcher accepts only the applicable value among `request`, `send-send`, `no-await`, `worker-offload`, `publish` |
| `--terminal` | `ordinary`, or `yield` for worker | §10.5/§10.8 handlers consume `ordinary`/`yield`; other cells are fixed to ordinary |
| `--channel-topology` | `routemesh` | `channel-echo-only` bootstrap consumes `routemesh`/`clientserver`; other S2S Channels are fixed to RouteMesh |
| `--codec` | Only `json` | Typed-payload configuration validation and serializer metadata |
| `--output` | `perf-results/<run-id>` | Run root for script and writer |
| `--run-id` | UTC label plus unique suffix | Script uses it for resources, logs and result identity; `[A-Za-z0-9_-]+` |
| `--endpoint-config` | Script-generated file | Actual endpoint manifest read by the standalone client |
| `--workload-config` | Omitted for standard echo | Production workload manifest consumed by the §23 script; separate from ordinary cells |
| `--config` | Required for role execution | One role config file read before that server executable starts |

For every payload, `run_perf.sh` executes all `ordinary/yield × SpotId 1/16` cells in §10.5,
both `ordinary/yield` cells in §10.8, and both `routemesh/clientserver` cells in §11.
`run_single.sh` selects one cell. A single-cell result does not establish full standard-matrix completion.

### 5.1 Role config and endpoint manifest

- **The script creates role config before starting servers.** Store, listener, topology and workload
  inputs needed at startup cannot depend on an endpoint file created afterwards.
- **A role executable reads one config file.** Passing endpoints and timeouts again through environment
  variables would create a second configuration owner.
- **The client manifest uses actual listener observations or verified reservations.** Input port `0`
  is not an address clients can connect to ([Listener identity][listener]).

Role config contains identity (`runId`, `cellId`, `role`, `roleInstance`), bind/advertise inputs,
Store provider/namespace/endpoint, ChannelName/topology/membership, `spotIds`/`actorIds`,
execution mode, workload and metrics listener. Unneeded object roles are not registered.
The manifest shares identity and config hash; `roles` is an array with one entry per process.

```json
{
  "runId": "20260906-example",
  "cellId": "s2s-channel-to-spot-request-echo-4096-example",
  "roles": [
    {
      "role": "channel",
      "roleInstance": 0,
      "streamEndpoint": null,
      "applicationTriggerUrl": "http://127.0.0.1:49152/app/perf/start",
      "metrics": {"transport": "http", "baseUrl": "http://127.0.0.1:49153"},
      "transportEndpoints": {"mesh": "tcp://127.0.0.1:49154"},
      "spotIds": [],
      "actorIds": []
    }
  ]
}
```

Ports illustrate format only; they are neither defaults nor a reserved range.
The CS connector consumes `streamEndpoint`, the server-driven standalone client consumes
`applicationTriggerUrl`, and the admin client consumes `metrics.baseUrl`.
`transportEndpoints` records actual server transport configuration/diagnostics, never trigger addresses.
Each server role also uses `applicationTriggerUrl` to start its window (§16).
Non-CS roles have `streamEndpoint=null`.
Each subscriber has an entry with `role=subscriber`, `roleInstance=subscriberId`.

### 5.2 Common workload values

Standard echo role config records `requestTimeoutMs=1000`, `correlationExpiryMs=1000`,
`settleTimeoutMs=5000`, `setupTimeoutMs=30000`, and `adminTimeoutMs=5000`.
Consumers are respectively public request calls, harness correlations, phase owners, script/setup
callers and HTTP clients. Record effective family send timeouts from public socket configuration
(standard: 1000ms; [owning contract][submit]).

Worker config records `minThreads=workerPoolSize`, `maxThreads=workerPoolSize`,
`maxQueueLength=4096`, `idleTimeoutMs=60000`, `workerTimeoutMs=requestTimeoutMs`, and effective
executor limits. Apply these only through each language's public worker options (§10.8).
Ordinary workloads are closed-loop: a stream starts its next operation after completion.
Inputs changing rate, bursts or Core/queue profiles belong only to the §23 manifest.

## 6. Standard Project Structure

- **Perf lives in execution projects separate from sample/E2E.** The public echo calls and measurement
  cost must be reviewable without business code.
- **The process executing measured calls owns their loop.** CS uses the client; server-driven cells
  use the source server, so server throughput is not duplicated in the trigger client.

```text
framework/languages/<lang>/perf/
|-- Shared/
|   |-- Contracts/
|   `-- Payload/
|-- Client/
|   |-- Program.*
|   |-- Scenarios/
|   `-- Support/
|-- Servers/
|   |-- SessionActorLocal/
|   |-- Session/
|   |-- Actor/
|   |-- Channel/
|   |-- Spot/
|   |-- ActorCaller/
|   |-- Publisher/
|   `-- Subscriber/
|-- ServerSupport/
|   |-- Metrics/
|   |-- Readiness/
|   |-- Logging/
|   `-- ProcessMetrics/
|-- scripts/
|   |-- run_perf.sh
|   |-- run_single.sh
|   `-- collect_env.sh
`-- perf-results/
```

Casing follows language convention consistently within each language.

### 6.1 Top-Level Responsibilities

| Location | Responsibility |
|---|---|
| `Shared` | §12 DTOs and payload pattern, §15 schema and histograms |
| `Client/Scenarios` | CS public connector calls or server-driven application triggers |
| `Client/Support` | Configuration, CS partitioning, phases, admin calls and result storage |
| `Servers/<Role>` | Public host configuration, typed scenario handlers and server workloads |
| `ServerSupport` | Application instrumentation, public status collection, readiness and process resources |
| `scripts` | Build, preflight, process start, barriers, original collection and cleanup |

`Program.*` handles only parsing, logging, DI/host configuration and scenario selection.
This structure includes no new runtime adapter or raw-frame processing helper.

### 6.2 The 10,000-Client Driving Model

CS partitions 10000 physical connectors among `client-count` processes.
For `N=connections`, `P=clientCount`, `i=clientIndex`, `q=floor(N/P)`, `r=N mod P`,
a process owns `q + (i < r ? 1 : 0)` connectors starting at ID `i*q + min(i,r)`.
`P <= N`; global client IDs do not overlap.

- **Bound connect/setup by connect-concurrency per process and start it once per connector.**
  Reconnection progress belongs to the [Connector contract][connector]; the pool adds no retry
  that hides failures. Record final readiness success/failure counts and setup latency separately.
- **Do not start measured load if preparation succeeds for less than 99%.** This is a CS connection
  readiness validity criterion, not an error-free echo or PS delivery criterion.
- **Use the same prepared connection set for warmup and measurement.** Use §4 drain/reset to avoid
  excluding connection-replacement cost in only one language.

Progress connector modes requiring public dispatch according to the [language connector interfaces][connector],
and record the actual mode. Add no native poller or binding completion pump.

Server-driven cells create no physical connectors. One source process runs `logicalStreams`
streams; the standalone client with `clientCount=1` only triggers the workload.
Spot cells use `streamId mod spotCount`; AC maps each stream ID to one ActorId.
CS binds one Actor per prepared connector ID. Record mapping and Actor count in config.
Section 13 owns slot/count semantics, and §15 owns aggregation.

### 6.3 Server Role Separation

| Executable role | Features and process boundary |
|---|---|
| `SessionActorLocal` | STREAM session and Object Server/Actor factory on the same local object node |
| `Session` | STREAM session and Object Client for remote CS; STREAM alone for the session baseline |
| `Actor` | Object Server, Actor factory and typed echo handler |
| `Channel` | Channel handlers and required public caller; Channel→Spot callers also register Object Client |
| `Spot` | Object Server, User Spot factory, local public driver and Spot handlers |
| `ActorCaller` | Session-less Object Client, direct Actor caller and dedicated return Channel Server |
| `Publisher` | Automatic Classic fanout publisher and application workload |
| `Subscriber` | Typed fanout handler and per-subscriber evidence |

- **Use role-specific execution projects and per-process admin endpoints.** Combining remote roles
  into one process changes the hop and resource-use comparison boundaries.
- **Register Location Store as a shared dependency.** It is the provider required by [object roles][mesh]
  and automatic discovery, not a reason to create a separate `Registry` server role.

Channel echo targets use the same `Channel` execution project. Subscribers are N independent
processes of the same executable, each with a distinct `roleInstance` and metrics endpoint.

### 6.4 Separating Support Code

- **Share payload generation, histogram, phase and correlation storage code only.** These are
  responsibilities of the measurement apparatus and do not change public-call kinds.
- **Keep request/send/Yield calls and completion recording visible in the scenario handler or loop.**
  A wrapper such as `EchoAsync` hides the distinctions needed to review completion boundaries.

## 7. Folder Structure Rules

Create folders only for repeated responsibilities. Do not add `Utils` or `Common` for a single file.

### 7.1 Client Folder

Scenario files correspond to §8.4 names. Comparison cells run as configuration of the same file.
`PerfRunPlan` owns CS ID partitioning, `ConnectionPool` public connection preparation/cleanup,
`ScenarioRunner` phases, `MetricsClient` admin calls and `ResultWriter` original storage.
If server code also needs correlation/in-flight instrumentation, keep one copy in `Shared`.

### 7.2 Server Folder

Each `Servers/<Role>` is a separate executable with `Program`, host configuration and typed handlers.
Create `Handlers` only as the number of handlers grows. Each role registers its common metrics endpoint.
The source role executing measured calls contains the workload and completion recorder.

### 7.3 Result And Log Folders

`perf-results` and per-run `logs`/`tmp` are generated, gitignored output.
Section 15 defines cell paths and rejection of overwrites.

## 8. Client Scenario Writing Style

A scenario shows where load starts and what counts as completion.
CS shows connector calls; server-driven scenarios show the HTTP trigger and source server workload location.

### 8.1 The Top Of A Scenario File

The initial comment states the measurement question, roles/processes, operation start/completion,
payload, mode/terminal/topology, Store requirement and reasons for null metrics.

### 8.2 Scenario Body

This is contract pseudocode for measurement flow, not an actual Framework API.
Actual language calls follow the interface links in §10 and §11.

```text
reserve one logical in-flight slot
record start immediately before the measured public call
invoke the public request or initial send once
observe the first request terminal or harness echo outcome
validate the echoed identity and payload
record window/settle outcome; release the slot
```

Send/send keeps correlation registration and the return handler visible.
A server-driven trigger's HTTP acknowledgement is recorded separately from this echo outcome.

### 8.3 Helper Usage Criteria

| Allowed responsibility | Reason |
|---|---|
| Payload generation/validation | Uses the same logical bytes and pattern |
| Latency, histogram and counter storage | Aggregates application measurements consistently |
| CS connection pool and phase control | Makes preparation and termination consistent |
| Recording a correlation's first result | Handles application identity for send/send |
| Admin HTTP and result writing | Collects outside measured calls |

Do not use helpers that wrap public request/send calls to hide completion meaning or accept runtime
objects. Physical retries, completion drain and reconnect belong to [Core][core-socket] and
[binding][binding-async], not perf support.

### 8.4 Naming

| Scenario | File/Class name |
|---|---|
| `cs-local-session-actor-echo` | `CsLocalSessionActorEchoScenario` |
| `cs-remote-session-actor-echo` | `CsRemoteSessionActorEchoScenario` |
| `s2s-channel-to-spot-request-echo` | `S2sChannelToSpotRequestEchoScenario` |
| `s2s-channel-to-spot-send-send-echo` | `S2sChannelToSpotSendSendEchoScenario` |
| `s2s-spot-to-channel-request-echo` | `S2sSpotToChannelRequestEchoScenario` |
| `s2s-spot-to-channel-send-send-echo` | `S2sSpotToChannelSendSendEchoScenario` |
| `spot-no-await-echo` | `SpotNoAwaitEchoScenario` |
| `spot-worker-offload-echo` | `SpotWorkerOffloadEchoScenario` |
| `actor-no-bind-request-echo` | `ActorNoBindRequestEchoScenario` |
| `actor-no-bind-send-send-echo` | `ActorNoBindSendSendEchoScenario` |
| `pubsub-fanout-echo` | `PubSubFanoutEchoScenario` |

Only casing may vary by language. Historical labels are not relabeled as standard results without
verified semantic equivalence. A pure Channel request differs from a STREAM session→Actor request.
Section 11 defines executable baseline names.

### 8.5 What A Client Must Never Do Directly

- **A standalone client creates no server host or handler.** This preserves process boundaries.
- **Judge success from replies, correlations and admin JSON.** Console text is not a result schema.
- **Start server-driven load only through application HTTP triggers.** This avoids assuming a STREAM
  connector can connect to a session-less ActorCaller or Publisher ([Session][session]).

## 9. Runner Script Writing Style

| Script | Responsibility |
|---|---|
| `run_single.sh` | Runs one scenario/payload/variant cell |
| `run_perf.sh` | Runs required cells sequentially and writes the run index |
| `collect_env.sh` | Collects public OS/runtime information and artifact provenance |

The script executes §4 phases and records generated config, process handles and container IDs.
Builds use the build-only lock shared by Java/Kotlin, released before process execution.
Section 20 owns port and Docker isolation. Cleanup never searches and kills processes by name or prefix.

CS clients use distinct indices and the same cell/reset identity. The script gathers §15 owner
originals without adding counts across cells. Originals with differing buckets or identity,
or missing required role originals, produce a failed result.
Measured loops and Framework calls do not live in shell scripts.

## 10. Standard Scenarios

Each table defines the measurement boundary for prepared steady echo. Sections 4–5 own common
workloads/windows; §14–15 own errors and null representation. Unless stated otherwise, Channels
use automatic RouteMesh and User Spots use `SpotWide` with Actor-free Spot direct workloads.
[User Spot execution mode][g-execution] configures which callbacks share execution authority;
ordinary/Yield semantics belong to the [execution contract][turn].
Actor/Spot creation, binding and warmup are recorded as setup, outside latency and KOPS.

### 10.1 `cs-local-session-actor-echo`

Measures the combined connector, session and Actor dispatch cost when session and Actor use the
same local object node, through the [session binding and original-reply contract][binding].

| Item | Measurement condition |
|---|---|
| Roles/processes | CS Client × clientCount, SessionActorLocal × 1; session Actor route and Actor owner share the same local object node |
| Load/mode | Physical connectors; `request`, ordinary; representative 1024 bytes |
| Completion/owner | Client: immediately before public request through validated typed echo on the original STREAM request |
| Preparation | Public manager prepares one Actor per connector ID; bind its Ref to that session |
| Location Store/Docker | Required for Object Server; run-dedicated Docker Redis |
| Null/unsupported | `actor.sourceAdmission.*` inapplicable without direct send; internal Spot metrics lack public observations; worker/fanout inapplicable |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use connector `Request(dto).Async<PerfEchoReply>()`, setup `Actors.BindOrGetAsync(ref)` and session `RelayAsync(payload)` ([connector][d-connector], [session][d-session], [Actor][d-actor]). |
| C++ | Use connector `request(dto).submit<PerfEchoReply>(callback)`, public `dispatch()`, `stream.actors().bind_or_get(ref)` and session relay, subject to the declaration check in §10.1 ([connector][c-connector], [session][c-session], [Actor][c-actor]). |
| Java | Use connector `request(dto).submit(PerfEchoReply.class)`, session `actors().bindOrGet(ref)` and `relay(dispatch,payload)` ([connector][j-connector], [session][j-session]). |
| Kotlin | Use connector wrapper `request<PerfEchoReply>(dto).await()`, `bindOrGetActor(ref)` and session wrapper `relay(dispatch,payload).await()` ([connector][j-connector], [session][k-session]). |
| Node.js | Use connector `request(dto).submit<PerfEchoReply>()`, session `actors.bindOrGet(ref)` and `relay(dispatch,payload)` ([connector][n-connector], [session][n-session]). |

C++ session relay has a declaration difference between the [exact Actor interface][c-actor] and
public-header `relay_request`, requiring the contract owner's reconciliation. If that language
cell cannot be implemented and verified, record `unsupported` with a declaration-mismatch reason
and do not count it as complete. Add no perf-specific raw codec. .NET also needs reconciliation
between the explicit-reply description in its [exact Session interface][d-session] and the
[original-reply observation in Session binding][binding]. Do not count relay admission as Actor echo
completion or invent an unconfirmed reply path in perf. These limitations apply to both languages
in §10.2; unresolved contract/declaration differences produce unsupported cells.

### 10.2 `cs-remote-session-actor-echo`

Measures the remote-hop and relay cost of placing session and Actor in separate processes.
Calls follow [Session binding][binding], with Actor count and payload matched to the local cell.

| Item | Measurement condition |
|---|---|
| Roles/processes | CS Client × clientCount, Session(Object Client) × 1, Actor(Object Server) × 1; distinct server PIDs |
| Load/mode | Physical connectors; `request`, ordinary; representative 1024 bytes |
| Completion/owner | Client: public request through validated original STREAM echo |
| Preparation | Prepare Actors on the Actor server and bind Refs to sessions; separate create/bind latency |
| Location Store/Docker | Both object roles require Docker Redis in the same run namespace |
| Null/unsupported | Same metric applicability/observation limits as §10.1; C++/.NET relay/reply contract checks required |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use connector `Request(dto).Async<PerfEchoReply>()`, setup `Actors.BindOrGetAsync(ref)` and session `RelayAsync(payload)` ([connector][d-connector], [session][d-session], [Actor][d-actor]). |
| C++ | Use connector `request(dto).submit<PerfEchoReply>(callback)`, public `dispatch()`, `stream.actors().bind_or_get(ref)` and session relay, subject to the declaration check in §10.1 ([connector][c-connector], [session][c-session], [Actor][c-actor]). |
| Java | Use connector `request(dto).submit(PerfEchoReply.class)`, session `actors().bindOrGet(ref)` and `relay(dispatch,payload)` ([connector][j-connector], [session][j-session]). |
| Kotlin | Use connector wrapper `request<PerfEchoReply>(dto).await()`, `bindOrGetActor(ref)` and session wrapper `relay(dispatch,payload).await()` ([connector][j-connector], [session][k-session]). |
| Node.js | Use connector `request(dto).submit<PerfEchoReply>()`, session `actors.bindOrGet(ref)` and `relay(dispatch,payload)` ([connector][n-connector], [session][n-session]). |


### 10.3 `s2s-channel-to-spot-request-echo`

Measures lookup, delivery and reply cost when the Channel process starts a remote request by
global SpotId. Targeting and completion refer to [Spot addressing][spot-address] and [Submit][submit].

| Item | Measurement condition |
|---|---|
| Roles/processes | HTTP trigger Client × 1, Channel(Object Client) × 1, Spot(Object Server) × 1 |
| Load/mode | Logical streams on Channel; `request`, ordinary; representative 4096 bytes |
| Completion/owner | Channel process: immediately before RequestToSpot through validated typed echo |
| Preparation | Manager-prepared User Spot `spotIds`, assigned by streamId mod spotCount |
| Location Store/Docker | Required for both Object Client/Server; run-dedicated Docker Redis |
| Null/unsupported | Physical connections and worker/actor/fanout metrics inapplicable; internal Spot mailbox/turn metrics lack public observations |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use `IZLinkSpotClient.RequestToSpot(spotId,dto).Async<PerfEchoReply>()` and a typed Spot request handler ([Spot interface][d-spot]). |
| C++ | Use `route_client_t.request_to_spot(spotId,dto).async<PerfEchoReply>()` and a typed Spot request handler ([Channel][c-channel], [Spot][c-spot]). |
| Java | Use `ZLinkRouteClient.requestToSpot(spotId,dto).submit(PerfEchoReply.class)` and a typed Spot request handler ([Channel][j-channel], [Spot][j-spot]). |
| Kotlin | Use route wrapper `requestToSpot<PerfEchoReply>(spotId,dto).await()` and a suspending Spot request handler ([Channel][k-channel], [Spot][k-spot]). |
| Node.js | Use public DI `ZLinkSpotOutbound.requestToSpot(spotId,dto).submit<PerfEchoReply>()` and a typed Spot request handler ([Spot interface][n-spot]). |

Node.js uses injected public Spot outbound. It does not supply absent `ZLinkRouteClient` members
with an internal cast. The declaration difference between these interfaces remains an owner check.

### 10.4 `s2s-channel-to-spot-send-send-echo`

Asks how completion rate, throughput and round-trip time of bidirectional sends differ from
requests in the same direction. [Send admission][submit] and [Channel selection][channel] remain
owned by their contracts.

| Item | Measurement condition |
|---|---|
| Roles/processes | HTTP Client × 1, Channel(Object Client+return Channel Server) × 1, Spot(Object Server) × 1 |
| Load/mode | Channel logical streams; `send-send`, ordinary; representative 4096 bytes |
| Completion/owner | Channel: correlation registration/just before first public send through echo validation in the return Channel handler |
| Return | Run/cell-specific ChannelName with this caller as its sole Server, configured during setup |
| Location Store/Docker | Run-dedicated Docker Redis for global Spot calls and automatic RouteMesh |
| Null/unsupported | Physical connections, worker, Actor and fanout inapplicable; internal Spot metrics unsupported |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use source `SendToSpot(...).Async()`, Spot `Outbound.SendToChannel(returnChannel,reply).Async()` and a Channel send handler ([Spot][d-spot], [Channel][d-channel]). |
| C++ | Use `route_client_t.send_to_spot(...).async()`, public route client `send_to_channel(returnChannel,reply).async()` and typed send handlers ([Channel][c-channel], [Spot][c-spot]). |
| Java | Call `sendToSpot(...).submit()` and Spot `outbound().sendToChannel(returnChannel,reply).submit()` with typed send handlers ([Channel][j-channel], [Spot][j-spot]). |
| Kotlin | Use route wrapper `sendToSpot(...).await()` and the Channel-send wrapper `await()` on Spot outbound ([Channel][k-channel], [Spot][k-spot]). |
| Node.js | Use injected public Spot outbound `sendToSpot(...).submit()` and Spot `outbound.sendToChannel(returnChannel,reply).submit()` ([Spot][n-spot], [Channel][n-channel]). |


### 10.5 `s2s-spot-to-channel-request-echo`

Asks how terminal choice and Spot count affect completed throughput, tail latency and progress of
other callbacks on the same Spot→Channel remote request. The [execution contract][turn] owns turn semantics.

| Item | Measurement condition |
|---|---|
| Roles/processes | HTTP Client × 1, Spot(Object Server+local public driver) × 1, Channel echo target × 1 |
| Required cells | `ordinary × 1`, `ordinary × 16`, `yield × 1`, `yield × 16` SpotId; all at each payload |
| Load/mode | Logical streams in Spot process; `request`; representative 4096 bytes |
| Execution | Actor-free `SpotWide` User Spot direct request handler; streams evenly assigned to SpotIds |
| Completion/owner | Spot handler: just before the public remote Channel request through reply validation after its terminal; driver RTT separate |
| Fixed comparison inputs | Remote Channel process configuration, payload, logicalStreams, inflight, deadlines and execution mode |
| Location Store/Docker | Run-dedicated Docker Redis for User Spots and automatic mesh |
| Null/unsupported | Exact suspended/resumed turns, resume latency and mailbox depth lack public observations; `spot.remoteCallLatency.*` and application Yield-call counts are measurable; worker/Actor/fanout inapplicable |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use Spot `Outbound.RequestToChannel(name,dto).Async<PerfEchoReply>()` or `.Yield<PerfEchoReply>()` with a typed Channel request handler ([Spot][d-spot], [Channel][d-channel], [terminals][d-common]). |
| C++ | Inside the Spot handler use injected `route_client_t.request_to_channel(name,dto).async<PerfEchoReply>()` or `.yield<PerfEchoReply>()` ([Channel][c-channel], [Spot][c-spot]). |
| Java | Use Spot `outbound().requestToChannel(name,dto).submit(PerfEchoReply.class)` or `.yield(PerfEchoReply.class)` ([Spot][j-spot], [Channel][j-channel]). |
| Kotlin | Use the public `awaitReply<PerfEchoReply>()` or `yieldReply<PerfEchoReply>()` bridge on the Spot outbound request ([Spot][k-spot], [Channel][k-channel]). |
| Node.js | Use Spot `outbound.requestToChannel(name,dto).submit<PerfEchoReply>()` or `.yield<PerfEchoReply>()` ([Spot][n-spot], [Channel][n-channel]). |

- **Do not funnel the workload into one Actor.** An [Actor queue claim][turn] would introduce a
  different experiment from the shared Spot gate comparison.
- **Record progress through application handler entries and completion counters.** Yield-call count
  is not actual turn-release count; do not infer internal times to report resume latency.

The local driver uses the per-language public Spot calls in §10.3 with `PerfDriveRequest`.
The handler makes one remote call above with the enclosed echo DTO. All four primary histograms
cover that same remote-call interval; handlers do not carry shared mutable business assumptions
across the wait.

### 10.6 `s2s-spot-to-channel-send-send-echo`

Measures cost and completion rate when a send from a Spot to a Channel returns to a separate
send handler on the original Spot. Application correlation follows [Spot outbound][spot] and
[one-way completion][submit].

| Item | Measurement condition |
|---|---|
| Roles/processes | HTTP Client × 1, Spot(Object Server+driver) × 1, Channel(Object Client) × 1 |
| Load/mode | Spot logical streams; `send-send`, ordinary; representative 4096 bytes |
| Completion/owner | Source Spot handler: correlation registration/just before Channel send through echo validation in the original Spot return handler |
| Return | Explicit source User SpotId in DTO `returnSpotId`; Channel handler makes a public send to that ID |
| Location Store/Docker | Run-dedicated Docker Redis for source User Spot and return caller Object Client |
| Null/unsupported | Physical connections, worker, Actor and fanout inapplicable; exact internal Spot metrics unsupported |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use Spot `Outbound.SendToChannel(...).Async()` and the Channel handler’s injected `IZLinkSpotClient.SendToSpot(returnSpotId,reply).Async()` ([Spot][d-spot], [Channel][d-channel]). |
| C++ | Use public `send_to_channel(...).async()` inside the Spot handler and Channel handler `route_client_t.send_to_spot(returnSpotId,reply).async()` ([Channel][c-channel], [Spot][c-spot]). |
| Java | Use Spot `outbound().sendToChannel(...).submit()` and Channel handler `sendToSpot(returnSpotId,reply).submit()` ([Channel][j-channel], [Spot][j-spot]). |
| Kotlin | Await the Spot outbound Channel send and return Spot send using their public `await()` wrappers ([Channel][k-channel], [Spot][k-spot]). |
| Node.js | Use Spot `outbound.sendToChannel(...).submit()` and the Channel handler’s injected `ZLinkSpotOutbound.sendToSpot(returnSpotId,reply).submit()` ([Spot][n-spot], [Channel][n-channel]). |

- **The source Spot handler returns after observing first-send admission.** Waiting there for the
  same Spot's return handler while retaining the turn would obstruct the [execution contract][turn].
  The driver waits for application correlation outside that turn, holding in-flight through the final outcome.
- **The DTO carries the return address.** Do not assume the Channel context supplies a source SpotId.

### 10.7 `spot-no-await-echo`

Measures the interval from a same-process public Spot client to an immediately echoing User Spot.
It uses [public Spot dispatch][spot-address] and is not interpreted as pure internal mailbox cost.

| Item | Measurement condition |
|---|---|
| Roles/processes | HTTP Client × 1, Spot(Object Server+local application driver) × 1 |
| Load/mode | Logical streams, `no-await`; ordinary request at caller, immediate typed reply at handler; representative 1024 bytes |
| Completion/owner | Spot process: just before local public RequestToSpot through validated echo; includes codec/local dispatch, excludes HTTP trigger |
| Preparation | Prepare User Spots with only the local Object Server eligible for placement; Actor count 0 |
| Location Store/Docker | Local User Spots also require run-dedicated Docker Redis |
| Null/unsupported | Remote call, worker, Actor and fanout inapplicable; mailbox depth and actual turn metrics lack public observations |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use `IZLinkSpotClient.RequestToSpot(spotId,dto).Async<PerfEchoReply>()` and a typed Spot request handler ([Spot interface][d-spot]). |
| C++ | Use `route_client_t.request_to_spot(spotId,dto).async<PerfEchoReply>()` and a typed Spot request handler ([Channel][c-channel], [Spot][c-spot]). |
| Java | Use `ZLinkRouteClient.requestToSpot(spotId,dto).submit(PerfEchoReply.class)` and a typed Spot request handler ([Channel][j-channel], [Spot][j-spot]). |
| Kotlin | Use route wrapper `requestToSpot<PerfEchoReply>(spotId,dto).await()` and a suspending Spot request handler ([Channel][k-channel], [Spot][k-spot]). |
| Node.js | Use public DI `ZLinkSpotOutbound.requestToSpot(spotId,dto).submit<PerfEchoReply>()` and a typed Spot request handler ([Spot interface][n-spot]). |


### 10.8 `spot-worker-offload-echo`

Asks how public CPU worker computation and result delivery add to local Spot echo cost.
Worker execution and terminals refer to [Submit §3][submit] and [execution][turn].

| Item | Measurement condition |
|---|---|
| Roles/processes | HTTP Client × 1, Spot(Object Server+local driver+Framework worker) × 1; no remote echo process |
| Load/mode | Logical streams, `worker-offload`; ordinary/yield comparison, default yield; representative 1024 bytes |
| Execution | Actor-free SpotWide User Spots; same local public driver as §10.7 |
| Completion/owner | Spot process: local RequestToSpot through validated echo after worker result; separate public worker-call interval |
| Workload | Identical worker-task-millis and §5.2 pool/executor settings |
| Location Store/Docker | Run-dedicated Docker Redis required for User Spots |
| Null/unsupported | Worker queue depth and internal Spot metrics lack public observations; submit→start/end→resume intervals null without a verified cross-worker clock domain; remote/Actor/fanout inapplicable |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use Spot context `RunCpuWorker(work).Timeout(...).Async()` or `.Yield()` and public worker options ([Spot][d-spot], [Worker][d-common]). |
| C++ | Use `run_cpu_worker(work).timeout(...).async()` or `.yield()` and `worker_options_t` ([Spot][c-spot], [Worker][c-common]). |
| Java | Use `runCpuWorker(work).timeout(...).submit()` or `.yield()` and `ZLinkWorkerOptions` ([Spot][j-spot], [configuration][j-config]). |
| Kotlin | Use `.kotlin().await()` or `.kotlin().yield()` on the Java worker call and Java public worker options ([Spot][k-spot], [configuration][j-config]). |
| Node.js | Use `runCpuWorker(work).timeoutMs(...).submit()` or `.yield()` and `ZLinkWorkerOptions` ([Spot][n-spot], [Worker][n-worker]). |

- **Callbacks run the same self-contained CPU workload without sleep.** Sleeping creates a different
  load from CPU execution.
- **Node callbacks return cloneable results without capturing a mutable collector.** Instrumentation
  must stay within the isolation boundary of the [public worker callback contract][n-worker].

Fix the workload to `xorshift32-v1`: start with unsigned 32-bit `x=0x12345678` and execute
`x ^= x << 13; x ^= x >>> 17; x ^= x << 5` with 32-bit arithmetic.
Every 1024 iterations, check monotonic elapsed time and cancellation; stop at or beyond the target duration.
Bootstrap fixes the target duration as a constant the callback can read independently.
Return `WorkerObservation` (§15) with start/end ticks, iteration count and checksum.
Record actual callback duration and iterations; a configured 5ms is not an observed 5ms.

Submit→start includes admission and dispatch overhead; end→caller continuation includes result delivery
and gate reacquisition. Record [ErrorKind][errors], without reconstructing worker-internal reasons.

### 10.9 `actor-no-bind-request-echo`

Measures lookup and remote-request cost when accessing global ActorId without session binding.
[Actor §5][actor] owns targeting and [Submit][submit] owns completion.

| Item | Measurement condition |
|---|---|
| Roles/processes | HTTP Client × 1, ActorCaller(Object Client) × 1, Actor(Object Server) × 1 |
| Load/mode | ActorCaller logical streams; `request`, ordinary; representative 4096 bytes |
| Completion/owner | ActorCaller: just before RequestToActor through validated typed echo |
| Preparation | One ActorId per stream; await public GetOrCreate, then use unbound Actors in Entry membership |
| Location Store/Docker | Run-dedicated Docker Redis required for Actor authority |
| Null/unsupported | Physical connections, Spot, worker and fanout inapplicable; `actor.sourceAdmission.*` inapplicable without send |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use `RequestToActor(actorId,dto).Async<PerfEchoReply>()` and a typed Actor request handler ([Actor interface][d-actor]). |
| C++ | Use `actor_client_t.request(actorId,dto).async<PerfEchoReply>()` and a typed Actor request handler ([Actor interface][c-actor]). |
| Java | Use `requestToActor(actorId,dto).submit(PerfEchoReply.class)` and a typed Actor request handler ([Actor interface][j-actor]). |
| Kotlin | Use Actor wrapper `requestToActor<PerfEchoReply>(actorId,dto).await()` and a suspending Actor request handler ([Actor interface][k-actor]). |
| Node.js | Use `requestToActor(actorId,dto).submit<PerfEchoReply>()` and a typed Actor request handler ([Actor interface][n-actor]). |


### 10.10 `actor-no-bind-send-send-echo`

Separately measures source admission and application echo round-trip cost for global ActorId send.
Measurement intervals refer to [Actor messaging][actor] and [source-local admission][submit].

| Item | Measurement condition |
|---|---|
| Roles/processes | HTTP Client × 1, ActorCaller(Object Client+return Channel Server) × 1, Actor(Object Server) × 1 |
| Load/mode | ActorCaller logical streams; `send-send`, ordinary; representative 4096 bytes |
| Completion/owner | ActorCaller: correlation registration/just before SendToActor through validated echo in the return Channel handler |
| Separate interval | Same call start through SendToActor terminal: `actor.sourceAdmission.*` |
| Preparation/return | One unbound Actor per stream; run/cell-specific RouteMesh return ChannelName with caller as sole Server |
| Location Store/Docker | Run-dedicated Docker Redis for object roles and automatic return Channel |
| Null/unsupported | Remote-mailbox admission timestamp lacks a public observation; physical connections, Spot, worker and fanout inapplicable |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use `SendToActor(actorId,dto).Async()` and public Channel client `SendToChannel(returnChannel,reply).Async()` injected into the Actor ([Actor][d-actor], [Channel][d-channel]). |
| C++ | Use `actor_client_t.send(actorId,dto).async()` and the Actor’s public route client `send_to_channel(returnChannel,reply).async()` ([Actor][c-actor], [Channel][c-channel]). |
| Java | Use `sendToActor(actorId,dto).submit()` and injected `sendToChannel(returnChannel,reply).submit()` in the Actor ([Actor][j-actor], [Channel][j-channel]). |
| Kotlin | Use Actor wrapper `sendToActor(actorId,dto).await()` and injected Channel wrapper `sendToChannel(returnChannel,reply).await()` ([Actor][k-actor], [Channel][k-channel]). |
| Node.js | Use `sendToActor(actorId,dto).submit()` and public Channel client `sendToChannel(returnChannel,reply).submit()` injected into the Actor ([Actor][n-actor], [Channel][n-channel]). |

The Actor responds with a public Channel send. Creating a BoundSession to respond changes the
no-bind premise. Remote source admission is not labeled local mailbox handoff.

### 10.11 `pubsub-fanout-echo`

Asks for one Publisher's local publication throughput and N Subscribers' actual receipt ratio and
latency. [Classic fanout][g-fanout] is the separate PUB/SUB facility sending events to prepared
subscribers; completion and delivery refer to the [owning contract][fanout].

| Item | Measurement condition |
|---|---|
| Roles/processes | HTTP Client × 1, Publisher × 1, Subscriber × subscriberCount; distinct subscriber PIDs |
| Load/mode | Publisher logical streams; `publish`, ordinary; representative 1024 bytes |
| Completion/owners | Publisher public-publish admission and each Subscriber typed handler's unique delivery, aggregated separately |
| In-flight | Limits publisher-local admission waits only; no subscriber ACK window |
| Preparation | Automatic discovery; per-subscriber public Ready and first warmup-marker receipt |
| Location Store/Docker | Run-dedicated Docker Redis required for standard automatic discovery |
| Null/unsupported | Echo completed/KOPS/echo latency inapplicable; one-way latency null without a verified clock domain; Spot/worker/Actor inapplicable |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use `IZLinkFanoutClient.Publish(channel,topic,event).Async()` and `IZLinkFanoutHandler<PerfPublishEvent>` ([Channel interface][d-channel]). |
| C++ | Use `publisher_t.publish(channel,topic,event).async()` and a typed fanout subscriber handler ([Channel interface][c-channel]). |
| Java | Use `ZLinkFanoutClient.publish(channel,topic,event).submit()` and `ZLinkFanoutHandler<PerfPublishEvent>` ([Channel interface][j-channel]). |
| Kotlin | Use `ZLinkKotlinFanoutClient.publish(channel,topic,event).await()` and a typed/suspending fanout handler ([Channel interface][k-channel]). |
| Node.js | Use public fanout client `publish(channel,topic,event).submit()` and a typed fanout handler ([Channel interface][n-channel]). |

One Publisher is the sole sequence issuer within the run. Warmup and measured ranges do not overlap.
Section 15 intersects successful measured publications with each subscriber's unique receipt set.
Report publish-admission ops/sec and subscriber-delivery ops/sec instead of echo KOPS.

Packet-name typed handler selection and topic scope refer to the [Framework API][api].
Do not assume per-subscriber transport topic filters for Classic fanout.
[Config 3 PS-A2][fanout] verifies handler selection by packet name.
Inject no late join, restart or reconnect into this steady cell. Record loss through deliveryRatio
and Framework failures through their actual public kinds.

## 11. Baseline Scenarios

Baselines use the §4 phases and §15 per-cell originals. Validate the apparatus first with
`session-echo-only`, and run both Channel baseline topologies as required cells.

### 11.1 `session-echo-only`

Asks for connector, codec and session cost of a STREAM request without an Actor hop.
A [public session callback][session] echoes the typed payload.

| Item | Measurement condition |
|---|---|
| Roles/processes | CS Client × clientCount, Session × 1; no Actor dispatch or object role |
| Load/mode | Physical connectors, `request`, ordinary; representative 1024 bytes, also run 4096 |
| Completion/owner | Client: just before public request through validated typed echo |
| Location Store/Docker | Not required for pure STREAM; add no automatic discovery |
| Null/unsupported | Actor, Spot, worker and fanout metrics inapplicable; infer no remote object behavior |

| Language | Public calls and exact interface |
|---|---|
| .NET | Use connector `Request(dto).Async<PerfEchoReply>()` and session typed handler `Client.Reply(reply).Async()` ([connector][d-connector], [session][d-session]). |
| C++ | Use connector `request(dto).submit<PerfEchoReply>(callback)`, `dispatch()` and public session `reply_packet(reply).async()` ([connector][c-connector], [session][c-session]). |
| Java | Use connector `request(dto).submit(PerfEchoReply.class)` and session `client().reply(reply).submit()` ([connector][j-connector], [session][j-session]). |
| Kotlin | Use connector wrapper `request<PerfEchoReply>(dto).await()` and session wrapper `reply(reply).await()` ([connector][j-connector], [session][k-session]). |
| Node.js | Use connector `request(dto).submit<PerfEchoReply>()` and session `client.reply(reply).submit()` ([connector][n-connector], [session][n-session]). |

### 11.2 `channel-echo-only`

Asks for topology cost by executing the same request without Spot/Actor through RouteMesh and
ClientServer. Refer to [Channel messaging][channel] and [ClientServer request/reply][clientserver].

| Item | Required RouteMesh cell | Required ClientServer cell |
|---|---|---|
| Roles/processes | HTTP Client × 1, Channel source × 1, Channel target × 1 | Same process separation |
| Configuration | Object role None; manual RouteMesh peer and Channel Client/Server | Manual ClientServer client and remote Server |
| Load/mode | Source logical streams, `request`, ordinary; representative 4096 bytes, also run 1024 | Same |
| Completion/owner | Source: just before public Channel request through validated typed echo | Same |
| Location Store/Docker | Not required without object roles or automatic discovery | Not required with manual endpoints |
| Null/unsupported | Physical connectors, Actor, Spot, worker and fanout inapplicable | Same; do not substitute separate Completion-connection bytes for this topology's reply bytes |

| Language | Public calls and exact interface |
|---|---|
| .NET | Both cells use `IZLinkRouteClient.RequestToChannel(name,dto).Async<PerfEchoReply>()` and `IZLinkRequestHandler` ([Channel][d-channel], [topology][d-config]). |
| C++ | Both cells use `route_client_t.request_to_channel(name,dto).async<PerfEchoReply>()` and a typed Channel request handler ([Channel interface][c-channel]). |
| Java | Both cells use `ZLinkRouteClient.requestToChannel(name,dto).submit(PerfEchoReply.class)` and a typed Channel request handler ([Channel interface][j-channel]). |
| Kotlin | Both cells use route wrapper `requestToChannel<PerfEchoReply>(name,dto).await()` and a suspending Channel handler ([Channel interface][k-channel]). |
| Node.js | Both cells use `requestToChannel(name,dto).submit<PerfEchoReply>()` and a typed Channel request handler ([Channel interface][n-channel]). |

Channel send measuring admission alone has a different result unit from this request baseline and
is not a required cell. Define delivery/completion units before adopting it as a separate measurement.

### 11.3 Local Spot Reference

`spot-local-echo` is a comparison-table reference to the local `spot-no-await-echo` cell in §10.7.
It creates no separate executable, CLI scenario, result or second completion count.
Its question, roles, per-language public calls, completion, Store and null conditions all refer to
that same §10.7 cell.

## 12. Message Contract

- **Framework messages use the default typed JSON serializer.** Section 15 fixes payload JSON
  representation so languages need no separate message codec to reconcile native byte-array defaults.
- **Echo returns received identity and payload unchanged.** Regeneration, compression or partial
  replies must not create false fast successes. Receivers validate length and the entire byte pattern.

| Application DTO | Use and purpose |
|---|---|
| `PerfEchoRequest` | Measured public request or initial send: identity, logical sequence and return address |
| `PerfEchoReply` | Typed request reply or return send: same identity and payload |
| `PerfDriveRequest` / `PerfDriveReply` | Local public Spot driver in §10.5/§10.6: whether the measured operation started and driver completion |
| `PerfTriggerRequest` / `PerfTriggerReply` | Phase start and acknowledgement on the separate application HTTP endpoint |
| `PerfPublishEvent` | Typed fanout event with sole-publisher sequence and fixed topic |
| `WorkerObservation` | Timing/workload evidence returned by a public worker callback |
| `PerfMetricsSnapshot` | §16 admin query or client original result |

Section 15.2 is the sole definition of field types, JSON, ticks and payload pattern.
Message registration uses stable packet names and typed handlers ([Framework API][api]).
`PerfDriveRequest` is a different application packet from an HTTP trigger or remote Channel echo.

## 13. Fairness Between The Request Style And The Send/Send Style

A **logical operation** is the workload unit started by one measured public call.
Reserve an in-flight slot before the public call and retain it through the request's first terminal
or the final send/send echo-correlation outcome, including source-admission waiting.
The local Spot driver in §4.2 submits this unit; nested public calls are not extra KOPS.

- **Do not count native DONTWAIT attempts, wait tokens or WRITABLE as operations.**
  [Core socket][core-socket] and the [binding completion owner][binding-async] own their progress;
  perf observes one public awaitable's result.
- **All languages use harness correlationId for send/send.** Two one-way calls form an application
  echo regardless of whether Framework request correlation is available.
- **Compare request and send/send with the same stream count, inflight, payload, placement and deadlines.**
  Unbounded send accumulation or a different return target measures a different completion cost.

Request terminal selection and conditions allowing remote work to remain belong to [Submit §9][submit].
The harness records validated public-reply success, public failure and cancellation as exclusive outcomes.
Register send/send correlation immediately before the first public send and fix its expiry at that same time.

| Observation | Harness record |
|---|---|
| Successful first-send admission | `messages.admitted`; AC also records sourceAdmissionMs; not echo success yet |
| First valid reply to a pending correlation | One echo success; §4 separates window/settle |
| First-send failure while pending | That public failure is final; do not count expiry afterwards |
| Deadline reached while pending | One `messages.timeout` and its subset `messages.expired`; harness `CorrelationExpired` |
| Additional reply to a successfully closed correlation | `messages.duplicateReply`; success count/histogram unchanged |
| Reply to a failed/expired correlation | `messages.lateReply`; original outcome unchanged |
| Correlation not issued in this cell | `messages.unknownCorrelation`; no existing outcome changed |
| Reply identity/payload mismatch | `messages.failed` and harness `PayloadMismatch` or `IdentityMismatch` |

Keep closed correlation identities and reasons through the final snapshot. If reply, failure and
expiry race, record only the first final outcome. `errors.byKind` describes outcomes and is not
added again to failure totals. Public request `DeadlineExceeded` and harness `CorrelationExpired`
are each counted once in `messages.timeout`, in separate Framework/harness error namespaces.

Reconcile an echo cohort with this equation. Counts use §15 decimal strings.

```text
messages.sent = messages.completed + messages.settleCompleted
              + messages.failed + messages.timeout
              + messages.cancelled + messages.unresolved
messages.expired <= messages.timeout
```

`sent` counts logical measured-call starts, not successful physical transmissions.
Section 15.4 separately reconciles publications. Failed/unresolved operations do not enter success latency.

If send/send echo arrives before the first-send terminal, preserve its echo timestamp. Release the
in-flight slot only after observing both the final echo outcome and first-send terminal, so a new
operation does not overlap a public call still awaiting admission. If that terminal remains absent at
the settle bound, do not count the echo again; fail the cell with a separate collection failure.

## 14. Metrics

Obtain metrics from application instrumentation, public host status, standard provider instruments
and public OS/runtime process APIs. Do not substitute approximations for values absent from
[Runtime monitoring][monitor] and [Runtime metrics][metrics]. Result metrics live in a `metrics`
object with dotted keys.

| Key or key group | Unit/format | Measurement boundary |
|---|---|---|
| `connections.requested/connected/failed` | count, U64 string | Final CS setup outcomes; preserved separately from measured-counter reset |
| `load.logicalStreams`, `load.inflightPerStream`, `load.inflight.max` | count, U64 string | Server-driven streams, configured per-stream cap and maximum total outstanding observed by the application |
| `messages.sent` | count, U64 string | Logical operations starting a measured public call inside the window |
| `messages.admitted` | count, U64 string | Successful public admission terminals of initial one-way sends; null for requests |
| `messages.completed`, `messages.settleCompleted` | count, U64 string | Window/settle successes in §4 |
| `messages.failed/timeout/cancelled/unresolved` | count, U64 string | Mutually exclusive cohort outcomes in §13 |
| `messages.expired/duplicateReply/lateReply/unknownCorrelation` | count, U64 string | Send/send correlation outcomes and additional observations; inapplicable to requests |
| `applicationMessages.request/send/reply/event` | count, U64 string | Public call starts or typed reply returns on the measured application path inside the window; not wire frames |
| `applicationPayloadBytes.request/send/reply/event` | bytes, U64 string | Logical payload bytes corresponding to each observation above |
| `throughput.kops` | number, kop/s | Owner's window echo successes/sec/1000; §15.4 covers CS aggregation |
| `throughput.messagesPerSec` | number, message/s | Sum of application message counts/sec; excludes native attempts, headers, fragments, handshakes, drivers and admin |
| `throughput.megabytesPerSec` | number, MiB/s | Sum of directional logical payload bytes/sec/1048576; includes both echo directions, not wire bandwidth |
| `latency.meanMs/p50Ms/p95Ms/p99Ms/maxMs` | number or null, ms | Window echo-success histogram `latencyMs` |
| `settle.latency.*` | number or null, ms | Settle echo-success histogram `settleLatencyMs` |
| `actor.sourceAdmission.latency.*` | number or null, ms | AC send start→successful source admission, derived from `sourceAdmissionMs` |
| `spot.remoteCallLatency.*` | number or null, ms | §10.5 call start→validated reply, including gate reacquisition/continuation; same interval as `latencyMs` |
| `spot.applicationYieldCalls` | count, U64 string | Actual Yield invocations in application handlers, not turn suspension count |
| `spot.applicationHandlerEntries` | count, U64 string | Application Spot handler entries receiving measured DTOs |
| `driver.issued/notStarted/failed` | count, U64 string | §4.2 local driver calls and pre-measurement-start refusal/failure; separate from KOPS |
| `driver.latency.*` | number or null, ms | Application interval from local driver public call to its result |
| `worker.callLatency.*` | number or null, ms | Immediately before public worker call→result at caller continuation |
| `worker.submitToStart.*` | number or null, ms | Same-clock-domain worker call start→callback start, including admission/dispatch |
| `worker.taskLatency.*` | number or null, ms | Callback monotonic start→end |
| `worker.resultToContinuation.*` | number or null, ms | Callback end→caller resumption, including result delivery/gate reacquisition |
| `messages.published/publishedInWindow/settlePublished` | count, U64 string | All/window/settle successful publications in the PS cohort; §15.4 |
| `fanout.subscriberCount` | count, U64 string | Independent Subscriber process count |
| `fanout.uniqueDelivered/deliveredInWindow/settleDelivered` | count, U64 string | Per-subscriber validated unique receipts in the publisher window-success set; §15.4 |
| `fanout.duplicateEvents` | count, U64 string | Additional receipts with the same measured identity; diagnostic independent of publisher-success intersection |
| `fanout.outOfCohortEvents` | count, U64 string | Unique sequences outside the final publisher window-success set; excluded from delivery numerator |
| `fanout.deliveryRatio` | number or null, ratio | Minimum per-subscriber uniqueDelivered/publishedInWindow |
| `fanout.publishOpsPerSec` | number or null, op/s | publishedInWindow/publisher measuredSeconds |
| `fanout.deliveryOpsPerSec` | number or null, event/s | Per-subscriber deliveredInWindow/its measuredSeconds; aggregate is the sum |
| `fanout.deliveryLatency.*` | number or null, ms | Just before publish call→subscriber handler entry, only with a verified shared clock domain |
| `fanout.settleDeliveryLatency.*` | number or null, ms | Same fanout interval for settle receipts, separate from window histogram |
| `process.cpuPercent` | number or null, % | Process CPU-time delta/monotonic measured time×100; one core is 100%, values may exceed 100% |
| `process.rssMb` | number or null, MiB | Maximum 100ms process RSS sample; actual sampling interval also recorded |
| `process.allocatedMb` | number or null, MiB | Measured allocation delta exposed by the runtime |
| `gc.gen0/gen1/gen2` | count string or null | Window deltas only where the runtime exposes these generations; do not relabel JVM collectors as .NET generations |
| `errors.byKind` | object of count strings | Public Framework ErrorKind outcomes observed by the aggregation owner |
| `errors.harness` | object of count strings | Application instrumentation, validation and correlation failures |
| `errors.language` | object of count strings | Argument/configuration errors without a public kind and language cancellation; preserve actual type |

Each latency `*` expands to `meanMs`, `p50Ms`, `p95Ms`, `p99Ms`, `maxMs`.
Keep scalar keys for inapplicable standard families as null per §15.5 rather than omitting them.
`driver.*` applies only to the auxiliary local driver in §10.5–10.6. The local public caller
interval in §10.7–10.8 is already primary latency and is not duplicated in driver histograms.
Physical connection metrics are null for server-driven cells; CS has `load.logicalStreams=null`.
Record CS in-flight through per-connector configuration and actual instrumentation.

### 14.1 Values Without Public Observations

These keys are null for lack of public observation even in the related workload; they are null as
inapplicable in other workloads. Do not copy host aggregates into per-Spot/worker values.

| Key | Reason |
|---|---|
| `spot.mailboxDepth.max/mean` | Public status exposes host aggregates only |
| `spot.suspendedTurns`, `spot.resumedTurns` | No public per-turn counters |
| `spot.resumeLatency.p95Ms/p99Ms` | Internal remote reply-ready timestamp is not public |
| `worker.pool.queueDepth.max/mean` | Public worker options are configuration, not queue-depth snapshots |
| `host.queueWaitLatency.p50Ms/p95Ms/p99Ms` | No public hook for exact pre-receive→handler queue wait |

### 14.2 Errors and Resources

`errors.byKind` uses names from [Framework error model §2][errors]. Match each language enum by
numeric value and normalize to `NotFound`, `AlreadyExists`, `TypeMismatch`, `NotConfigured`,
`Rejected`, `Unavailable`, `CapacityExceeded`, `DeadlineExceeded`, `ShuttingDown`, `ProtocolError`,
`InvalidOperation`, `DataLost`, `InternalFailure`. Perf creates no new kind.
Do not reconstruct internal route/worker causes through string parsing.

Harness keys are `CorrelationExpired`, `PayloadMismatch`, `IdentityMismatch`, `UnknownCorrelation`,
`DuplicateReply`, `SettleIncomplete`, `PhaseMismatch`, `SchemaMismatch`, `CollectionFailure`.
These are not Framework enums. Synchronous language errors without a public kind use `errors.language`.
If caller and target both observe a remote failure, result totals use only the §15 owner's
observation; target diagnostics remain in its original role file.

Keep resource figures per process; do not add CPU percentages/RSS and call the result a host metric.
Record reasons when GC or common generations are unavailable. Supplement with actual names and
units in `runtimeMetrics` for JVM heap/non-heap, Node event-loop delay and similar observations.

## 15. Result File Format

### 15.1 Per-Cell Files and Identity

- **Preserve each cell's originals in its own directory.** A new scenario, payload, terminal or
  placement in the same run must not overwrite earlier results.
- **Fail when an output path already exists.** Repeated execution needs a new runId or §23 repetition
  so comparison inputs remain distinguishable.

```text
perf-results/<run-id>/
|-- index.json
|-- summary.txt
`-- <scenario>/
    `-- <payload-bytes>/
        `-- <variant>/
            |-- config.json
            |-- endpoints.json
            |-- role-configs/
            |-- result.json
            |-- summary.txt
            |-- client-<index>.json
            |-- server-<role>-<instance>.json
            |-- publisher-sequences.json
            |-- subscriber-<id>-sequences.json
            `-- logs/
                |-- client-<index>.log
                |-- server-<role>-<instance>.log
                `-- message-flow-<role>-<instance>.log
```

Sequence files are PS-only. Do not enable extra tracing in ordinary measurement merely to create
message-flow files. Section 21 separates diagnostic runs from standard performance results.
`variant` is `<mode>-<terminal>-<topology>-s<spotCount>-n<subscriberCount>-<configHash>`.
Use `na` for inapplicable counts/topology in paths. `configHash` is the full lowercase SHA-256 hex
of comparison-input JSON UTF-8 bytes fixed before phase start. Preserve those exact input bytes in config.

Comparison inputs include language, mode, terminal, topology/discovery, execution mode, Spot/Actor
mapping rules/counts, subscriber count, connection/stream partition, in-flight, timeouts, worker workload/options,
CPU/memory/runtime options, serializer, and §23 workload hash/repetition. Exclude runId, PIDs,
dynamic ports and output paths from the comparison hash; retain them as environment metadata.
Generate concrete run/cell SpotIds and ActorIds after fixing the hash; hash placement/partition rules
instead of those ID strings. `cellId` is this relative directory within the run.

### 15.2 DTO Field Types and JSON

These declarations are **contract pseudocode for common application DTOs, not actual Framework APIs**.
All JSON property names use the lowerCamelCase shown. Schema version is integer `2`.

```text
U64 = decimal JSON string               // 0..18446744073709551615; "0" or digits without leading zeroes
I64 = decimal JSON string               // -9223372036854775808..9223372036854775807; signed ticks
Index = JSON integer                   // 0..2147483647; stream/client/subscriber index
Number = finite JSON number            // No NaN/Infinity; store calculations before display rounding
Text = JSON string                     // UTF-8; case-sensitive identity
Phase = "warmup" | "measured"

Identity {
  runId: Text                           // Run resource identity
  cellId: Text                          // Relative scenario/payload/variant directory in this run
  resetSeq: U64                         // Warmup 0; positive after measured reset
  phase: Phase                         // Separates warmup and measured cohorts
}
PerfEchoRequest extends Identity {
  clientId: Index                       // Global CS connector ID or source logical stream ID
  sequence: U64                         // Issued per clientId by source; never reused within cell
  correlationId: Text                   // cellId/phase/clientId/sequence; harness identity
  sentTicks: I64                        // Monotonic nanoseconds immediately before measured call
  clockDomainId: Text                   // Identifies the epoch/unit of these ticks
  returnSpotId: Text | null             // Only send/send requiring Spot return
  returnChannel: Text | null            // Only send/send requiring caller-specific Channel return
  payload: Text                         // Canonical padded Base64 of logical bytes
}
PerfEchoReply extends Identity {
  clientId: Index
  sequence: U64
  correlationId: Text                   // Copied unchanged from request
  receivedTicks: I64                    // Target handler's local monotonic time; diagnostic only
  clockDomainId: Text                   // Domain of receivedTicks; RTT uses caller's own clock
  payload: Text                         // Echo received Base64 unchanged
}
PerfDriveRequest {
  echo: PerfEchoRequest                 // Measured operation requested by local driver
}
PerfDriveReply {
  started: boolean                     // False if handler receives it after window end
  echo: PerfEchoReply | null            // Request result; null for send/send admission ACK or no start
}
PerfTriggerRequest extends Identity {}  // Starts configured phase once; no batchSize
PerfTriggerReply extends Identity {
  accepted: boolean
  state: "started" | "alreadyStarted" | "rejected"
  configHash: Text                      // Comparison-input hash applied by role
  reason: Text | null                   // Required on rejection; null on success
}
PerfPublishEvent extends Identity {
  sequence: U64                         // Sole Publisher issues throughout the entire run
  topic: Text                           // Fixed "perf.echo"
  sentTicks: I64
  clockDomainId: Text
  payload: Text
}
WorkerObservation {
  startedTicks: I64
  endedTicks: I64
  clockDomainId: Text
  iterations: U64
  checksum: JSON integer                // xorshift32 result, 0..4294967295
}
```

Logical payload byte `b[i]` is `(31*i + 17*floor(i/251) + 29) mod 256`.
`payloadSize` counts these bytes, excluding Base64 expansion, JSON properties and Framework headers.
Base64 is an application field representation, not a manual codec for the whole message.
Use the same default typed JSON serializer without per-packet encoders/decoders.
Snapshot `serializedMessageBytes` is an array of `{direction:Text, packetName:Text,
logicalPayloadBytes:U64, observedSerializedBytes:U64|null}`. Direction is one of
`request|send|reply|event`; separate DTO/payload combinations use separate rows.
If public observation is unavailable, observedSerializedBytes is null with a reason. Do not
serialize a message twice in the measured path to obtain this observation. Logical bytes are not wire bytes.

Elapsed time, deadlines and retention use a monotonic clock consistent with the [clock contract][liveness].
Ticks are **nanoseconds**, converted from the native clock frequency.
Clock metadata types follow. Nullable alignment fields are inapplicable to own-process RTT;
a common-domain claim supplies evidence and an error bound.

```text
ClockMetadata {
  source: Text
  nativeFrequencyHz: U64
  ticksUnit: "ns"
  clockDomainId: Text
  scope: "process" | "host" | "aligned-hosts"
  alignmentMethod: Text | null
  maxErrorNs: U64 | null
  validFromTicks: I64 | null
  validThroughTicks: I64 | null
  evidence: Text[]                      // Public OS/runtime references or alignment artifact paths
}
```
Unix timestamps are UTC display values only, never elapsed-time inputs.

Declare a common domain only with public OS/runtime evidence that epoch and unit are shared.
Even within a process, do not subtract worker clocks with different epochs.
Across hosts, leave one-way latency null unless alignment method, validity interval and error bound
are established. RTT uses two caller-clock readings, never remote receivedTicks subtraction.
Node bigint and all 64-bit values are decimal strings, never narrowed to JSON numbers.

### 15.3 Histograms and Percentiles

Application latency histograms use this format. Do not merge public providers' own histograms
with these application histograms.

```json
{
  "unit": "ms",
  "ticksUnit": "ns",
  "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
  "counts": ["0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0"],
  "overflow": "0",
  "count": "0",
  "sumNs": "0",
  "maxNs": null,
  "percentileMethod": "nearest-rank-bucket-upper-bound"
}
```

- **Buckets are `[0,b0]`, then `(b[i-1],b[i]]`, with noncumulative counts.** Every sample must enter
  exactly one bucket or overflow to support aggregation.
- **Estimate a percentile from the upper bound of its nearest-rank bucket.** Use the first bucket
  containing sample rank `ceil(p*count)`. Compute p50/p95/p99 rank with integer arithmetic
  `ceil(q*count/100)` for `q=50,95,99`, without narrowing count to floating point.
  Even a p50 below 0.1ms is reported as a quantized `0.1`
  estimate, not an exact observation.
- **If the selected rank is in overflow, the percentile is null.** Capping it at the last bound
  would understate the tail. Record `HISTOGRAM_OVERFLOW` and `lowerBoundMs=1024`.
- **Do not reconstruct mean/max from buckets.** Compute them from exact integer `sumNs`, `count`
  and `maxNs` over all valid samples, including overflow.

`count=sum(counts)+overflow`. Count fields are U64; integer overflow fails the cell.
`sumNs` is an arbitrary-precision nonnegative decimal string, without overflow.
`maxNs` is U64 or null; no samples means null with `NO_SAMPLES`.
`meanMs=sumNs/count/1000000`; `maxMs=maxNs/1000000`.
When sample count is zero, all percentiles, mean and max are null with `NO_SAMPLES`.

Merge counts/sums and take the maximum of max values only for matching units, bounds, cohorts and
intervals. Do not average process percentiles or means. Differing bucket originals fail with
`SchemaMismatch`. Keep `latencyMs` separate from `settleLatencyMs`.
Derive `spot.remoteCallLatency.*` from the identical §10.5 `latencyMs` interval without maintaining
a second histogram. Preserve other intervals with these histogram keys. One mapping of latency
keys to sample sets prevents collectors from merging different intervals.

| Histogram key | Metric prefix and samples |
|---|---|
| `latencyMs` / `settleLatencyMs` | `latency.*` / `settle.latency.*`; window/settle echo successes |
| `sourceAdmissionMs` | `actor.sourceAdmission.latency.*`; AC initial-send admission terminals inside window |
| `driverLatencyMs` | `driver.latency.*`; local driver completions inside window |
| `workerCallLatencyMs` | `worker.callLatency.*`; worker calls completing inside window |
| `workerSubmitToStartMs` | `worker.submitToStart.*`; the same callbacks as those worker calls |
| `workerTaskLatencyMs` | `worker.taskLatency.*`; the same callbacks as those worker calls |
| `workerResultToContinuationMs` | `worker.resultToContinuation.*`; the same callbacks as those worker calls |
| `fanoutDeliveryLatencyMs` / `fanoutSettleDeliveryLatencyMs` | `fanout.deliveryLatency.*` / `fanout.settleDeliveryLatency.*`; §15.4 window/settle unique intersections |

All successful samples above belong to the measured cohort. Do not mix auxiliary settle samples
into primary echo histograms. Inapplicable histograms also carry null plus a reason.

### 15.4 Aggregation Owners and Result Schema

| Family | Primary owner/original | Aggregation |
|---|---|---|
| CS and session baseline | `client-<index>.json` | Sum echo counts/histograms; throughput is the sum of client count/measuredSeconds rates |
| Channel→Spot and Channel baseline | Source `server-channel-<instance>.json` | Source completion, correlation and clock |
| Spot→Channel and Spot local/worker | Source `server-spot-0.json` | Source handler/caller interval in §10; no additional echoes for HTTP triggers/drivers |
| AC | `server-actorCaller-0.json` | ActorCaller request/correlation and source admission |
| PS | `server-publisher-0.json` and all `server-subscriber-<id>.json` | Separate publish and per-subscriber delivery; sequence intersection below |

The process owning each public send/request start or typed reply return records that application
message and byte observation once. An incoming callback is not a second send of the same message.
Include additional sends actually called by application code, such as CS session relay, in their
direction. For `throughput.messagesPerSec` and `throughput.megabytesPerSec`, sum each measured
role’s window count/bytes divided by its own measuredSeconds. Exclude trigger clients. This
application rate scope differs from primary echo KOPS aggregation; record both scopes.
Exclude local drivers, HTTP and internal service/control records.
Originals record each role's own window and `messageCountScope="application-call-boundaries"`.

All snapshots share this structure. `Object` means JSON object; `[]` means JSON array.

```text
PerfMetricsSnapshot {
  schemaVersion: JSON integer           // 2
  runId: Text; cellId: Text; resetSeq: U64
  language: "dotnet" | "cpp" | "java" | "kotlin" | "node"
  role: Text; roleInstance: Index
  configHash: Text
  phase: "setup" | "warmup" | "reset" | "measured" | "settle" | "complete"
  window: {
    startedAtUnixMs: I64 | null         // Display only; null before start
    endedAtUnixMs: I64 | null
    startTicks: I64 | null
    endTicks: I64 | null
    measuredSeconds: Number | null     // (endTicks-startTicks)/1e9 on the same owner clock
    settleSeconds: Number | null
  }
  clock: ClockMetadata                 // §15.2
  serializedMessageBytes: Object[]      // Typed rows defined in §15.2
  metrics: Object                      // §14 dotted keys; counts are U64
  histograms: Object                    // §15.3 records or null
  nullReasons: Object                  // §15.5 JSON pointer → reason
  publicStatus: Object | null          // Actual public host snapshot; retain language fields/enums
  publicMetrics: Object[]              // name, kind, unit, labels, value; preserve owning metric names/units
  runtimeMetrics: Object               // Language process supplements with name/unit/type/value
  provenance: Object                   // §19 environment/artifact/PID information
}
PerfResult {
  schemaVersion: JSON integer           // 2
  runId: Text; cellId: Text; configHash: Text
  language: Text; scenario: Text
  configFile: Text; endpointsFile: Text // Relative paths within cell
  status: "valid" | "failed" | "invalid" | "unsupported"
  baselineEligible: boolean             // §19 performance-baseline eligibility
  reasons: Object[]                     // code, message, sourceFile
  metricOwners: Text[]                  // Primary original relative paths
  ownerWindows: Object                 // Original filename → its window
  measuredSeconds: Number | null       // Value for one primary owner; null+MULTIPLE_OWNERS for multiple CS owners
  aggregation: {
    rateMethod: "single-owner" | "sum-owner-rates"  // Primary echo rate
    applicationRateMethod: "sum-role-rates"
    fanoutDeliveryRateMethod: "sum-subscriber-rates" | null // Inapplicable outside PS
  }
  metrics: Object; histograms: Object; nullReasons: Object
  clients: Text[]; servers: Text[]      // Required original relative paths
  processes: Object[]                  // References to per-process resource/clock/public-status originals
}
```

Public status 64-bit fields also use §15.2 representation; the exact interface defines original types.
Public metric `value` is a string for integer declarations and a finite number for floating declarations.
Do not change histograms, labels or units and export them as the same provider metric.
`index.json` is `{schemaVersion:2, runId, cells:[{cellId, resultFile, status}]}`.
The run summary has one row per cell and no cross-cell throughput total.

The PS Publisher preserves the measured starting-sequence range and successful publication set.
`published=publishedInWindow+settlePublished`; both are successful public admissions of the measured cohort.
Reconcile `sent=published+failed+timeout+cancelled+unresolved`.
`messages.completed`, echo `latency.*` and `throughput.kops` are null with `NOT_APPLICABLE`.

Sequence files use the measured Identity from §15.2. Each range is inclusive, sorted,
disjoint and a maximal contiguous interval.

```text
Range { first: U64; last: U64 }
PublisherSequences extends Identity {
  attemptedRanges: Range[]              // All publish calls started during measurement
  windowSuccessRanges: Range[]          // Admission successful inside publisher window
  settleSuccessRanges: Range[]          // Admission successful in settle; excluded from ratio denominator
}
SubscriberSequences extends Identity {
  subscriberId: Index
  windowRanges: Range[]                 // Validated first receipt inside subscriber window
  settleRanges: Range[]                 // First receipt in settle; disjoint from windowRanges
  duplicateEvents: U64
  nullReasons: Object                  // §15.5 JSON pointers and reasons
  timingEvidence: ReceiptTiming[] | null // Null plus reason without a verified common clock domain
}
ReceiptTiming {
  sequence: U64
  sentTicks: I64; publisherClockDomainId: Text
  receivedTicks: I64; subscriberClockDomainId: Text
  receivedIn: "window" | "settle"
}
```

Publisher success sets exclude failed sequences. Final delivery aggregation intersects each
subscriber's windowRanges/settleRanges **only with windowSuccessRanges**. The ratio denominator
is `publishedInWindow`. Preserve settle publish successes separately without adding them to this denominator.
Per-subscriber `uniqueDelivered=deliveredInWindow+settleDelivered` is the size of that intersection.
If the same sequence arrives in both intervals, only the first receipt is unique; later receipts are duplicates.
Out-of-cohort receipts are outOfCohortEvents, outside unique delivery and histograms.
Each subscriber uses its monotonic window/settle intervals and retains evidence bounding start skew.
Zero denominator produces null with `ZERO_DENOMINATOR` and an `invalid` cell.
Missing delivery alone creates no Framework error.

Timing evidence has one row per unique receipt. Final latency histograms use only that intersection
and separate window/settle. Include evidence cost in actual subscriber resource use and record
collection method and retained bytes in provenance.

### 15.5 Null and Reason

- **Distinguish inapplicability, unsupported observation and no samples through null plus a reason.**
  Zero is reserved for a value actually observed as zero.
- **Distinguish an unsupported metric from an unimplemented scenario.** Internal metric nulls are
  valid, but a cell unable to execute a required public call is `unsupported` and not complete.

`nullReasons` uses a value's JSON pointer as key. Example:

```json
{
  "/metrics/spot.mailboxDepth.max": {
    "code": "PUBLIC_OBSERVATION_UNSUPPORTED",
    "reason": "Host capacity exposes no per-Spot mailbox depth",
    "owner": "spec/server/06-observability/01-runtime-monitoring"
  }
}
```

Reason codes are `NOT_APPLICABLE`, `PUBLIC_OBSERVATION_UNSUPPORTED`, `RUNTIME_METRIC_UNSUPPORTED`,
`CLOCK_DOMAIN_UNVERIFIED`, `NO_SAMPLES`, `HISTOGRAM_OVERFLOW`, `ZERO_DENOMINATOR`,
`MULTIPLE_OWNERS`, `PHASE_NOT_STARTED`, `COLLECTION_FAILED`.
Every null metric, histogram and window value has a nonempty reason.
DTO return addresses, replies and trigger reasons explicitly declared nullable follow their field comments.
A failed required collection produces `COLLECTION_FAILED` and a failed cell, not an unsupported metric.

## 16. Server Metrics Endpoint

Each role exposes application-owned admin HTTP endpoints; these are not Framework built-in URLs.
Endpoints use only public runtime status/reset and application collectors.
HTTP is the standard runner transport; other admin transports are outside these standard-schema cells.

| Endpoint | Consumer, input and observation |
|---|---|
| `GET /perf/ready` | Script queries stage-specific readiness evidence; starts no workload |
| `POST /perf/reset` | Coordinator supplies `{runId,cellId,resetSeq}` after drain |
| `GET /perf/stats` | Collector receives the current §15 PerfMetricsSnapshot |
| `POST /app/perf/start` | Application trigger client starts a phase with §15 PerfTriggerRequest; separate listener from metrics URL |

Application start opens measurement windows on receivers and also starts workloads on source roles.
CS servers receive window starts, but only CS clients generate connector load.
Script stdin/stdout JSON control pipes govern CS client phases using the same Trigger DTO and reset
acknowledgement. These control messages are client-application interfaces, not Framework APIs.
Acknowledge receiver phase starts before starting sources/CS clients. Record each trigger send and
acknowledgement on the coordinator’s single monotonic clock to bound observed start skew. Without
common-clock evidence, exact inter-process start differences are null with reasons.

### 16.1 Readiness

`GET /perf/ready` returns this common application DTO.

```text
PerfReady {
  runId: Text; cellId: Text; role: Text; roleInstance: Index
  infrastructureReady: boolean        // Listener and required public topology ready
  objectsReady: boolean               // Required public create/bind results observed; true if inapplicable
  consumersReady: boolean             // PS: all subscriber warmup-marker evidence
  ready: boolean                      // Conjunction of the stage-specific conditions above
  observedAtUnixMs: I64               // Display only
  evidence: Object[]                   // kind, source, observedValue; public status/typed reply/marker
  reasons: Text[]                     // Conditions not yet ready
}
```

Server start checks infrastructureReady. Warmup starts after infrastructureReady and objectsReady;
the measured barrier additionally requires consumersReady. Do not require ready=true before PS
markers and thereby prevent warmup itself.

Topology/object preparation refers to [Runtime monitoring][monitor], [Spot preparation][spot-address]
and [Session bind][binding]. Host startup alone does not establish ready ClientServer candidates
([Channel readiness][channel]). Subscribers return both public Ready and actual marker receipt
([Config 3][fanout]). Echo cells also confirm one probe echo for each prepared target, outside
warmup measurements. PS uses the marker above instead of an echo.

### 16.2 Reset and Snapshot

The reset response is `{ok:boolean,runId:Text,cellId:Text,role:Text,roleInstance:Index,resetSeq:U64,
applicationResetAtUnixMs:I64,capacityEpoch:U64|null,reason:Text|null}`.
Repeating the same resetSeq returns the saved acknowledgement without resetting counters again.
Older sequences, a different cell, or reset during active measurement return HTTP 409 with a nonempty
reason. Invalid JSON/field types return HTTP 400 without state changes. Success uses HTTP 200.

After application reset, observe public capacity-reset results and include the epoch in acknowledgement.
CS clients without a host runtime have capacityEpoch=null with an inapplicability reason.
Do not present this as an instantaneous reset across processes. Preserved capacity values and epoch
semantics refer to [Runtime metrics §3][metrics].

The exact public status/reset interfaces belong to [.NET][d-status], [C++][c-status], [Java][j-status],
[Kotlin][k-status] and [Node.js][n-status]. A trigger client without Framework records only application
and process values.

A snapshot includes requested cell/resetSeq, phase, window/settle counters and histograms, public
capacity and process figures. A low-cost sampler collects during measurement; serialize HTTP snapshots
and merge histograms during report. Admin endpoint failures remain collection failures.

### 16.3 Observation Cost

- **The hot path updates application counters and latency recorders only.** Per-message HTTP, console
  output or full public-status queries would make generation/observation a bottleneck.
- **Include receipt-evidence cost in process resources.** Hiding PS sequence-validation cost would
  undermine comparable subscriber-throughput conditions.

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
|-- ZLink.Framework.Perf.ActorCallerServer/
|-- ZLink.Framework.Perf.PublisherServer/
|-- ZLink.Framework.Perf.SubscriberServer/
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
|-- actor-caller-server/
|-- publisher-server/
|-- subscriber-server/
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
|-- actor-caller-server/
|-- publisher-server/
|-- subscriber-server/
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
|-- actor-caller-server/
|-- publisher-server/
|-- subscriber-server/
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
|-- actor_caller_server/
|-- publisher_server/
|-- subscriber_server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

The C++ implementation uses release build artifacts. If the core runtime or bindings runtime path
is older than the source, the runner must fail. A performance figure must not be interpreted from a
stale runtime.

## 18. Implementation Order

Language runners align public calls and results in this order. These are implementation dependencies,
not claims that execution or performance validation has completed.

1. Implement typed JSON DTOs, cell schema, clocks, histograms, error mapping and isolation scripts.
2. Connect public-status readiness, reset barriers, application triggers and window/settle recording.
3. Validate connector, phase and aggregation behavior with `session-echo-only`.
4. Configure `cs-local-session-actor-echo` and `cs-remote-session-actor-echo`.
5. Configure both required RouteMesh and ClientServer cells of `channel-echo-only`.
6. Configure `spot-no-await-echo` local and use its same result for the local baseline reference.
7. Configure Channel→Spot request and send/send.
8. Configure Spot→Channel request ordinary/Yield × SpotId 1/16 and send/send.
9. Configure worker ordinary/Yield and ActorId no-bind request/send-send.
10. Connect Classic fanout subscriber readiness and sequence originals.
11. Check required matrices and §22 interface observations through `run_perf.sh`.
12. Compare public calls/result schemas with other languages; contract owners review declaration differences.

Section 23 is a separate experiment requiring a workload manifest and production environment.
Section 2.1 candidates are separately designed once their scope is approved; they are not hidden
prerequisites for standard implementation.

## 19. Regression And Comparison Criteria

Compare results from the same environment, scenario, payload, terminal and placement.
Do not impose an arbitrary initial threshold; retain throughput, p99, errors and resource use together.

`provenance` records commit hash, Core/binding/Framework versions, artifact hashes, actual loaded paths,
build mode, CPU model/effective processors/quota/cpuset/executor maximum, memory limit, OS/kernel/container,
FD limit, role PID/host/endpoints, serializer name/version/options and clock evidence.
Config records actual connection/stream counts and partition, payload, duration, warmup, inflight,
deadlines, Spot/Actor mapping, topology/discovery and worker settings.

| Result status | Meaning and comparison use |
|---|---|
| `valid` | Required originals and cohort reconciliation are complete; workload outcomes observed |
| `failed` | Public-call, validation, phase or collection failure observed; preserve reasons/counts |
| `invalid` | Comparison premises fail, such as zero denominator, inadequate preparation or incompatible schemas |
| `unsupported` | Required public calls/declarations could not be confirmed/executed in that language; not counted complete |

Only `valid` echo cells with zero failure, timeout, cancellation, unresolved and validation errors
are adopted as successful echo baselines with `baselineEligible=true`.
PS has no lossless contract, so missing delivery alone is not an error; a valid publish denominator
and subscriber originals support comparison with the recorded ratio.
A comparison plan must state the minimum acceptable deliveryRatio for adopting a PS baseline;
if absent, `baselineEligible=false`. Never save figures containing errors as error-free baselines.

Summary also identifies client CPU saturation, subscriber evidence cost, clock error, null metrics
and histogram quantization. PS results with loss or §23 saturation results do not establish ordinary
echo completion guarantees.

## 20. Operating System And Execution Environment

- **Use public port-0 listener observations or verified port reservations.** Avoid sample/E2E port
  collisions; example numbers are not an exclusive perf range.
- **Each run requiring Store uses a dedicated Docker Redis container and per-cell namespaces.**
  Local Actors/Spots and automatic discovery use the [Store contract][mesh] and need isolated data.
  Host Redis fallback is not allowed. Prepare this Redis once before the first Store-dependent
  cell, retain it between cells, and clean up its exact container ID when the run ends or aborts.
- **Cleanup targets only this run's exact PIDs/process handles and container IDs.** Concurrent samples,
  E2E runs and user processes must remain untouched.
- **Java/Kotlin share the repository's build-only lock.** Prevent collisions within their shared build
  root without retaining the lock for process lifetimes ([isolation reference][sample-isolation]).

Record container image digest, container ID, actual mapped port, provider version and namespace in config.
Each cell uses fresh application processes and never reuses a Store namespace.
Only Store-free manual Channel/session baselines have no Docker dependency.
Keep Location Store and Relocation Store contracts distinct ([Location][g-store], [Relocation][g-relocation-store]).

The script checks FD limits, ephemeral ports, listen backlog, TCP TIME_WAIT policy, CPU/memory limits
and client/server host placement, reporting insufficiency as preflight failure.
It does not automatically modify global OS settings. Same-host loopback results include CPU contention.
Record cross-host placement with §15.2 clock evidence.

Readiness, liveness and shutdown refer to [Channel][channel], [liveness][liveness] and
[host shutdown][relocation]. Perf does not tune 5s/15s liveness or the ClientServer ready-wait cap.
Record PAUSED and not-ready separately and compare connection changes under saturation with the contracts.
Do not hide outcomes through timeout increases, manual reconnect or propagation sleeps.

## 21. Prohibited

- **Do not compensate for public-API completion at another layer.** Raw sockets, private runtime,
  reflection, second pollers and retry/reconnect state would measure a different path from the owner contract.
- **Do not create required metrics through per-message logs or internal tracing during measurement.**
  This changes observation cost and the public boundary. If needed, preserve file logs in a separate
  diagnostic cell using [message-flow tracing][flow], apart from standard performance results.
- **Do not omit reply validation or failure recording.** Broken echoes or failures in success latency
  would invalidate completion comparisons.
- **Do not change timeouts, budgets, retry counts or workloads within a comparison.** Making failures
  disappear through changed conditions does not demonstrate a root-cause correction.
- **Turn comparisons carry no mutable business assumptions across waits.** Application races unrelated
  to the [Yield contract][turn] must not contaminate the result.
- **Do not label debug builds, stale runtimes or fake backends as release baselines.** Section 19's
  actual execution environment is the comparison evidence.

## 22. Completion Criteria

Use only runner CLI, application admin/trigger JSON, typed replies, handler evidence, public
status/reset and stored result files for these checks. Do not judge internal queue, permit or
transport implementation state.

**Matrices and public calls**

- Running `run_perf.sh` produces 1024/4096 results for each §8.4 name and independently queryable results for all four §10.5 cells.
- Running `channel-echo-only` records distinct RouteMesh/ClientServer topologies and source/target PIDs in their results.
- Running `session-echo-only` and local Spot baselines records the §11 boundaries, with local Spot referencing a single §10.7 result.
- Sending a CS request yields an original STREAM reply with matching identity/payload, without create/bind setup time in the echo histogram.
- Running no-bind Actor cells records configured global ActorIds and public request/send results, without session-binding evidence.
- Running request/send-send with matching inputs records the same logical in-flight definition and separate send-admission/echo-completion metrics.
- Running worker cells records the public callback checksum, iterations, actual callback time and selected ordinary/Yield value.

**Phases and results**

- Requesting reset while warmup calls remain produces no measured-start acknowledgement.
- Starting measured load after all participants return the same resetSeq exposes that identity and a monotonic window in every original.
- Echoes completing after window end increment only settle counts/histograms and do not change throughput completion counts.
- Observing a valid reply, public failure and expiry for one correlation reconciles to one final outcome, with additional replies in separate counters.
- An explicit parameter without a consumer in that cell, or an invalid mode/codec input, returns a preflight error.
- Running different cells produces independent config/result/original files; reusing a cell path returns an overwrite error.
- Merging original histograms reproduces §15.3 counts/sums/max and percentile estimates, with a null reason for overflow percentiles.
- Metrics lacking public observations return null and a specific reason, without labeling host aggregates as Spot mailbox values.
- Window, setup or collection failure exposes its source file, actual public kind or harness reason and count in summary.

**Fanout, isolation and capacity**

- After PS warmup, each Subscriber exposes Ready and marker-receipt evidence.
- Collecting PS results allows ratios to be recomputed from publisher-window-success/subscriber-unique intersections, with echo KOPS null.
- Without common-clock-domain evidence, process one-way latency is null while delivery count and ratio remain queryable.
- A Store-dependent run exposes Docker container ID and cell namespace in config, and cleanup stops only that run's owned resources.
- Executing a §23 manifest records public capacity snapshots and outcomes by topology, without internal permit-handoff judgments.

## 23. Measuring Production Values For Core HWM And The Application Job Queue

This section compares Core byte-budget and Framework host-queue candidates under production workloads.
[Framework API][api], [Application job queue][queue], [Runtime monitoring][monitor] and
[Runtime metrics][metrics] own configuration, limits and pressure semantics.
Perf records public settings, snapshots and completion evidence.

### 23.1 Fixed Workload And CPU Matrix

`--workload-config` owns all workload values for this experiment. Reject duplicate CLI/manifest
inputs for the same value. The manifest identifies these inputs and consumers.

| Manifest input | Consumer |
|---|---|
| `scenario`, `payloadDistribution`, `requestOneWayRatio` | Application generator: packet-kind/logical-byte proportions |
| `logicalStreams` or `connections`, `inflight`, `ratePerSecond`, `burstRatePerSecond`, `burstDurationMs` | Corresponding CS/source generator: steady/burst load |
| `handlerCpuWork`, `handlerIoWork` | Public application handler/worker: fixed CPU/I/O ratio |
| `warmupSeconds=30`, `measuredSeconds=60`, `repetitions=5`, deadlines and settle values | Phase owner; recorded explicit inputs |
| `requestedProcessors=[4,8,16]`, `cpuQuota`, `cpuset`, `executorMaximum` | Process/container execution and public executor configuration |
| `memoryLimitBytes`, `runtimeOptions`, `gcOptions` | Process/container execution |
| `coreProfiles`, `coreBudgetCandidatesBytes`, `applicationQueueProfiles`, `manualQueueCandidates` | Public host configuration builder |
| `pauseThresholdPercent`, `resumeThresholdPercent` | Public application-queue configuration; record 80/60 defaults and actual values |
| `capacitySampleIntervalMs=100` | Application public-status sampler |
| `targetCpuRange`, `minThroughput`, `maxP99Ms`, `maxDeadlineMissRatio`, `maxMemoryBytes`, `lossPolicy`, `minDeliveryRatio`, `safetyMargin` | Production-candidate report judgment; minDeliveryRatio is PS-only |

Fix units and valid ranges in the manifest; exact interfaces own applicable public options.
`ratePerSecond` means logical operations/sec; burst duration uses monotonic milliseconds.
`lossPolicy` is `lossless` or `observe-delivery`; Classic fanout uses the latter.

Alongside requested CPU count, record runtime constrained count, affinity/cpuset count,
quota/period-derived count and executor maximum. Read effective processor count from public status
under the [API contract][api]; do not equate it with nominal vCPU count.
These are Auto-profile observation expectations for those effective values.

| Effective processors | Compact | LowLatency | Balanced | Throughput |
|---:|---:|---:|---:|---:|
| 4 | 128 | 256 | 512 | 1024 |
| 8 | 256 | 512 | 1024 | 2048 |
| 16 | 512 | 1024 | 2048 | 4096 |

Check manual `1`, `2147483647` and invalid `0`, negative and overflowing inputs through the
[API validation contract][api]'s public startup results. Perf does not instrument internal
pre-bind ordering. Confirm Core manual/profile precedence through public effective snapshots as well.

### 23.2 Measurement Phases And Reset Baseline

Each repetition follows §4 warmup drain→reset acknowledgement→measured→settle.
Run at least 30s warmup and 60s measurement five times, with steady/burst intervals fixed in the manifest.
If the coefficient of variation exceeds 5%, mark results unstable and retain originals.
Do not extend the same run's duration/timeouts to turn it into a pass.

Record public capacity snapshots before/after reset: configured values, current gauges, pressure
state/current pause duration, epoch, peaks, transitions, cumulative pause and configuration failures.
Compare preservation, rebasing and reset with [Runtime metrics §3][metrics].
Perf does not infer the internal epoch assignment of concurrent events.

Exact job-queue wait p50/p95/p99 is null per §14.1; do not copy sender RTT into it.
Sampled queue-gauge distributions include sample interval and `sampled=true`.

### 23.3 Selecting A Core HWM Budget

Compare Auto profiles and finite positive budget candidates from the manifest on the same workload.
Record public configured/effective budgets, applied HWM, accounted/completion-accounted values,
blocked ratio, active directional queues, process RSS/heap, outcomes and latency.
Do not interpret ABI-reserved fields as active operating values; refer to [monitoring][monitor]
for field semantics. Judge Core budgets separately from process RSS.

Select the smallest budget meeting manifest throughput/latency/memory conditions, then apply the
explicit operator margin. Interpret completion by topology. Compare RouteMesh's separate Completion
connection and ClientServer's single FIFO/HWM/PAUSED with [Queue §3][queue] and
[ClientServer §5][clientserver]; do not require identical progress guarantees.

### 23.4 Selecting A Manual Application Job Queue Limit

Choose manual candidates from public permits-in-use samples and observed peaks.
[Application job queue][queue] owns their definitions; do not add active-handler or async-wait counts.

```text
candidateMaxQueuedApplicationJobs =
    sampled permits-in-use percentile or observed burst peak
    + operator-selected safety margin
```

For each candidate preserve original names/units for effective limit, reserved/queued/in-use/peak,
capacity waits/duration, pressure state, pause/resume thresholds, current/cumulative pause,
transitions and flow-state configuration failures. Interpret these with public handler start,
completion and error evidence.

Do not merge shared-capacity wait, owner FIFO saturation and worker capacity across their
[error/pressure boundaries][queue] into one “HWM error”. Internal invariants about oldest-source
handoff, batch/1:N reservation and permit leaks remain the owner's contract-test responsibility.
A public gauge snapshot within limits does not prove those internal invariants.

### 23.5 Pass Conditions

Production-candidate judgments use §22 public observations and manifest thresholds.
Steady and burst phases each meet throughput, p99, deadline-miss and memory conditions.
Require zero missing/duplicate application deliveries only for `lossless` workloads.
Classic fanout uses ratio, per-subscriber counts and the specified minDeliveryRatio.

Record observed public in-use/peak relative to effective limits, preserving capacity-wait/pressure
counters. Do not replace internal permit-leak/source-handoff verification with a perf pass condition.
Preserve `zlink.host.core_hwm.*` and `zlink.host.application_job_queue.*` instrument names, units and
labels according to their [owning metric contract][metrics].

### 23.6 Values To Record

- Workload manifest bytes/hash, per-cell repetition and actual CPU/memory/runtime/GC configuration
- Requested/effective processors and constrained-count/cpuset/quota/executor-maximum evidence
- Public configured/effective Core/queue values, sampling interval/count and peaks
- Pause/resume thresholds, pressure state and current/epoch pause/transition/configuration-failure values
- Outcomes, completion rate, latency/RTT, reasons for null exact queue wait and topology interpretation
- Per-role resources, source/subscriber evidence, selected candidate/margin and threshold judgments

[Document list][docs] · [Common specification][common] · [Scenario E2E][e2e]

[docs]: ../../../../framework/doc/README.en.md
[common]: ../../../../framework/doc/framework/common/README.en.md
[e2e]: ../../../../framework/doc/framework/common/e2e/README.en.md
[submit]: ../../../../framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.en.md
[turn]: ../../../../framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.en.md
[queue]: ../../../../framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.en.md
[errors]: ../../../../framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.en.md
[api]: ../../../../framework/doc/framework/common/spec/server/00-foundation/06-framework-api.en.md
[monitor]: ../../../../framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.en.md
[metrics]: ../../../../framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.en.md
[flow]: ../../../../framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.en.md
[session]: ../../../../framework/doc/framework/common/spec/server/04-session/01-stream-session.en.md
[binding]: ../../../../framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.en.md
[channel]: ../../../../framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.en.md
[clientserver]: ../../../../framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.en.md
[listener]: ../../../../framework/doc/framework/common/spec/server/02-channel-transport/04-network-listener-identity.en.md
[liveness]: ../../../../framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.en.md
[spot]: ../../../../framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.en.md
[spot-address]: ../../../../framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.en.md
[mesh]: ../../../../framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.en.md
[actor]: ../../../../framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.en.md
[relocation]: ../../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.en.md
[fanout]: ../../../../framework/doc/framework/common/e2e/config-3-pubsub.en.md
[sample-isolation]: ../../../../framework/doc/framework/common/sample/README.en.md
[connector]: ../../../../framework/doc/framework/common/spec/stream-connector/32-stream-connector.en.md
[core-socket]: ../../../../core/doc/spec/core/socket/README.en.md
[binding-async]: ../../../../bindings/doc/spec/async-execution-model.en.md
[http-dotnet]: ../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/02-configuration-host.en.md
[http-cpp]: ../../../../framework/doc/framework/common/spec/server/languages/cpp/60-http-hosting.en.md
[g-turn]: ../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.en.md#spot-turn
[g-spot]: ../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.en.md#spot
[g-spot-id]: ../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.en.md#spot-id
[g-execution]: ../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.en.md#user-spot-execution-mode
[g-fanout]: ../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.en.md#classic-fanout
[g-store]: ../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.en.md#location-store
[g-relocation-store]: ../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.en.md#relocation-store
[d-session]: ../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.en.md
[d-channel]: ../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/04-channel-messaging.en.md
[d-spot]: ../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.en.md
[d-actor]: ../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/06-actors.en.md
[d-common]: ../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/01-common-runtime.en.md
[d-config]: ../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/03-configuration-topology.en.md
[d-status]: ../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.en.md
[c-session]: ../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/06-stream-session.en.md
[c-channel]: ../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.en.md
[c-spot]: ../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.en.md
[c-actor]: ../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.en.md
[c-common]: ../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.en.md
[c-status]: ../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/08-monitoring.en.md
[j-session]: ../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/stream-session.en.md
[j-channel]: ../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/channel-messaging.en.md
[j-spot]: ../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/spots.en.md
[j-actor]: ../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/actors.en.md
[j-config]: ../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/configuration-host.en.md
[j-status]: ../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/monitoring.en.md
[k-session]: ../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/stream-session.en.md
[k-channel]: ../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/channel-messaging.en.md
[k-spot]: ../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.en.md
[k-actor]: ../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.en.md
[k-status]: ../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.en.md
[n-session]: ../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.en.md
[n-channel]: ../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.en.md
[n-spot]: ../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.en.md
[n-actor]: ../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/05-actors.en.md
[n-worker]: ../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/06-stream-worker.en.md
[n-status]: ../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/03-location-observability.en.md
[d-connector]: ../../../../framework/doc/framework/common/spec/stream-connector/languages/dotnet/03-stream-connector.en.md
[c-connector]: ../../../../framework/doc/framework/common/spec/stream-connector/languages/cpp/03-stream-connector.en.md
[j-connector]: ../../../../framework/doc/framework/common/spec/stream-connector/languages/java/03-stream-connector.en.md
[n-connector]: ../../../../framework/doc/framework/common/spec/stream-connector/languages/typescript/03-stream-connector.en.md
