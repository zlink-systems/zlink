<!-- framework-adapter-nav:start -->
[Document list](../../../README.en.md) | [Common spec](../README.en.md) | [Common sample](../sample/README.en.md)
<!-- framework-adapter-nav:end -->

# Framework Scenario E2E Tests

This document organizes the e2e tests that confirm each language
implementation of ZLink Framework **runs correctly even on a server
that looks exactly like a real deployment.**

E2E is the standard for verifying the contract defined in the common
spec and finding implementation gaps. A new public API isn't added
based only on an e2e scenario or another language's implementation.
If a scenario seems to need a new API, first check whether there's a
contract basis in the common spec.

Even though both are verification, e2e differs in character from a
contract test or a sample.

- A contract test nails down the promise of each individual API
  quickly, in-process.
- A sample shows the normal flow a user can follow as-is.
- E2E goes one step further. With a real shared location store, real
  location resolution, multiple providers, and process boundaries
  actually split apart — that is, **conditions like a real
  deployment** — it confirms whether the feature behaves as intended.

## 1. Classification Principle — Config-Centric

E2E doesn't list features flatly. It treats **a server configuration
(config) that looks like a real deployment as one unit**, and verifies
detailed behavior on top of it the way a real user would. Each config
is an independently-runnable app just like a sample project — it
starts the server configuration once, then runs several client
scenarios in sequence.

### Selection Criteria

A scenario isn't picked by "does it not overlap with an existing
test." It's picked by **is it a flow a real user takes in a realistic
deployment configuration.**

- It's fine for assertions to overlap with an existing
  unit/contract/in-process test. The distinguishing point isn't
  novelty of assertion but realistic deployment context and
  sample-level public API usage.
- Even the same feature can behave differently once a real shared
  store, real resolution, multiple nodes, and process boundaries are
  involved. That's exactly the spot being looked at.

### Code Writing Rules

- E2E imitates the actual usage flow. A client scenario doesn't call
  framework internal APIs directly — like a real user, it calls the
  **HTTP endpoint of the role server app that provides the feature.**
  A structure calling a driver/test-runner server endpoint that just
  runs the scenario for you isn't treated as the real-usage flow.
- The client uses the per-language HTTP client wrapper. In `.NET`,
  use `ZLinkHttpClient`; don't call e2e app endpoints directly with a
  raw `HttpClient`. For scenarios that verify the stream connector
  itself, or that must observe the moment a value changes — state
  change, event, push — use the client stream connector as the public
  client surface.
- Client code doesn't directly use the channel/fanout/spot framework
  client, framework host configuration, or test-only helpers. For
  example, a `.NET` client doesn't use `IZLinkChannelClient`,
  `AddZLinkFramework`, `Host.CreateDefaultBuilder`, reflection
  bypasses, or private/internal API access.
- A framework call like request/send/publish/resolve lives inside the
  actual role server app that provides the feature. The server
  receives the user's request through an app endpoint, and inside it
  runs the real feature via the public framework API. Don't move
  existing client verification code into a separate project like
  `Server/Driver`, `ScenarioRunner`, or `TestHost` to have it perform
  framework calls instead.
- Work that changes the server configuration — process start/stop,
  restart, scale-out/in, failover — can be handled in `run_e2e.*` or
  client support code. But this code must not perform framework
  messaging calls in its place. A framework messaging call must live
  inside the actual role server endpoint.
- Split client scenarios by file, and briefly describe at the top of
  each scenario file what it verifies. Write the scenario body so the
  connector or HTTP client call flow is visible as much as possible,
  and don't let a verification helper hide the core flow. A scenario
  file must not end by calling a single endpoint that delegates the
  whole scenario, like `/run`, `/scenario/all`, or `/execute`.
- Don't repeat the same HTTP query dozens of times at short intervals
  to confirm a state change. If the server can announce it via push,
  connect the client stream connector first, and use the HTTP call
  only to trigger the state change. Verification cross-checks the push
  payload the connector received together with the actual role
  server's evidence/log.
- For a case like Pub/Sub where the verification target isn't the
  client stream session but the fanout delivery a separate subscriber
  role server received, the bounded `/evidence/wait` marker the
  subscriber handler left can be used as the success criterion. Here
  the evidence must be what the actual subscriber role server left,
  and the client triggers the publish via the publisher endpoint and
  then checks each subscriber's marker.
- Verification directly expresses the combination of what the client
  can see and the evidence/log the actual role server left. Don't
  substitute feature verification with evidence left only by a driver
  dedicated to running the scenario.
- Even if a feature an E2E scenario requires doesn't exist in a
  specific language, don't add a new public API right away unless
  there's a public-contract basis in the spec or the common framework
  spec/guide. Another language's implementation is only reference
  material for comparing contract interpretation, not a basis for a
  new contract by itself.
- An item that can't be implemented for lack of a public-contract
  basis isn't filled in with a workaround — an internal helper, a
  private API, or manipulating frame bytes directly. Leave it as a
  public-contract gap in the feature-map, and split off the needed
  draft/spec/guide review as separate work.

### Message Naming Principle

E2E message names follow the same message-naming principle as the
common sample. E2E has a wider verification scope than a sample, but
the payload exchanged between client and server must read by the same
standard as the public example the user sees.

For a payload called with request and awaiting a response, use `Req`
and `Res` as a pair. This standard applies to channel request, route
request, stream request, and HTTP request all alike. For a
one-way send payload with no response, use `Msg`, and for a payload
the server pushes to the client stream/session, use `Notify`.

Even if the business name looks like an event, `Req`/`Res` is correct
if the call method is request/reply. For example, an e2e payload that
requests a state change and awaits the processing result is named
like `StatusChangedReq` and `StatusChangedRes`. Conversely, a state-
change notification the client receives as a server push is named
like `StatusNotify`.

### Lifetime/Placement Scenario Terminology

Scenarios where server count and process state change use the
following meanings distinctly. Different names mean different
contracts to verify, so one scenario isn't called by multiple terms.

- **scale-out** is adding a running node. It doesn't mean an action
  that automatically changes who handles existing requests.
- **scale-in** is removing a running node. State handoff that needs
  logical continuity and host shutdown are distinguished as
  `Relocate`, and a bounded cleanup with no new relocation as
  `Shutdown`.
- **restart** is stopping and starting again a process of the same
  application role. In automatic topology using a Location Store, a
  new RID is issued after the prefix at every lifecycle, and the
  previous RID isn't reused.
- **replacement** is a new process taking over the same application
  role as a stopped node. Replacement in automatic topology also uses
  a new RID/lifecycle. Verification that reuses the same RID is kept
  only for role `None` in manual topology, which has no Store
  descriptor.
- **failover** is another already-running node continuing to accept
  work it can handle after one node terminates abnormally. It doesn't
  mean the stopped node's state is automatically transferred.
- **Actor relocation** is the explicit action of changing which side
  handles a specific Actor via the public relocation contract. When
  host `Relocate` starts a relocation, it's distinguished as a
  **maintenance handoff.**
- **recovery** means the whole procedure that returns the service to
  a normal state after a failure — restart, replacement, rejoin,
  replay.

Adding a MeshNode doesn't automatically change the owner of an
existing Spot or actor. A new node can become a placement candidate
for a Spot or actor newly created afterward, following the public
placement input and policy. Changing an existing owner requires an
action under a separate contract, like an explicit Actor relocation or
a `Relocate` maintenance handoff. So a scale-out scenario verifies
keeping the existing owner and new placement, and an Actor-move
scenario separately verifies the state/mailbox/bound-session handoff
contract.

## 2. Standard Project Structure

Each config is placed at the per-language standard location as an e2e
app separate from the sample. Server and client are built as apps with
real process boundaries, and every language keeps the same-meaning
folder structure. Even if the per-language build-tool name and file
extension differ, role separation, scenario-file separation, file
classification, and the evidence/wait method keep the same meaning.

The E2E app uses the structure below. When porting a scenario to
another language, don't copy the file location as-is — place the same
role's code according to the responsibility below.

Each language implementation follows the role boundary this document
and each config document defines. For example, if provider and
workflow are different deployment roles, build them as separate
runnable projects regardless of language — don't turn one server into
both roles by just changing an option. Client scenario code and shared
execution support code are also split into different responsibilities.

Example:

```text
framework/languages/<lang>/e2e/<Config>/
|-- Shared/
|-- Server/
|   |-- <Role>/
|   |   |-- Program.*
|   |   |-- <Role>HostFactory.*
|   |   |-- <Config>.<Role>.<project>
|   |   |-- Configuration/
|   |   |-- Endpoints/
|   |   |-- Handlers/
|   |   `-- Infrastructure/
|   `-- <OtherRole>/
|-- Client/
|   |-- Program.cs
|   |-- Scenarios/
|   `-- Support/
|-- run_e2e.sh
|-- feature-map.ko.md
`-- README.ko.md
```

`README.ko.md` is placed when supplementary explanation is needed per
config. The `framework/doc/framework/common/e2e/config-*.ko.md`
documents are the standard for common scenario definitions and
completion criteria.

The execution method resembles sample smoke — instead of a test
framework directly building a host in the same process, `run_e2e.*`
starts server processes in order, confirms port readiness, and then
runs the client scenarios. Scenarios like scale/failover let the same
script start or stop additional processes.

For how a role server endpoint is used, refer to `PubSub`'s and
`RegistrationCodec`'s client flow. The client directly calls endpoints
of real role servers like publisher/subscriber/main, and framework
features run inside the server endpoint. However, if an older config's
file location differs from this section's folder classification, don't
follow that old location as-is. A structure where a separate driver
server is started and the client hands "run the whole scenario" to
that driver isn't this document's standard structure.

### 2.1 Local E2E Wait Standard

Every framework e2e runner uses the same wait standard for local runs.
The standard values are kept as explicit config constants at the top
of each `run_e2e.sh`. Environment variables can only be used for a
slow CI or a diagnostic override, and the default completion evidence
must be a run that passed with these values, without override.

The defaults are as below.

| Item | Default | Meaning |
|------|--------|------|
| local readiness timeout | 3 seconds | Max time to wait for a newly started local process's port, health, and readiness |
| local readiness poll interval | 0.1 seconds | Interval for re-checking readiness |
| route settle | 5 seconds | Bounded-wait ceiling for confirming route status and application evidence stabilize after confirming public readiness. Success isn't judged by sleep alone |
| scenario settle | 3 seconds | Cleanup-grace ceiling used after a previous operation's terminal and evidence cleanup. Doesn't judge the readiness or success of the next scenario |
| HTTP probe/admin/evidence request timeout | 3 seconds | Max time for one local HTTP probe such as `/health`, `/evidence`, `/admin/*`, control ping |

A local e2e that isn't ready within these values isn't passed by
extending the wait time. First find and fix the cause — startup order,
readiness endpoint, location-row registration, route propagation,
stale process/port, an old build artifact, lifecycle cleanup. A long
wait is not recognized as a completion condition, because it makes a
bug get discovered later.

A timeout that verifies the scenario itself does a long operation —
client scenario process timeout, whole child-group timeout,
shutdown/recovery — is named separately from the readiness/settle
standard above. This kind of timeout is the test process's ceiling or
part of the behavior under verification, not a readiness value waiting
for a local process to become ready. A request where the server waits
for a scenario event, such as a bounded evidence wait, follows the
same principle. A simple evidence-snapshot request uses the 3-second
HTTP standard, but a bounded wait waiting for an event to appear is
split off into a separately-named scenario wait value.

### 2.1.1 Standard Runner Output And Interrupt Cleanup

Each language's `run_e2e_all.sh` prints summary lines in the same
shape so the runner immediately knows how far along it is. Each
config's detailed log flows through as before, but the aggregate
runner briefly summarizes start, config completion, and overall
completion.

The default output shape is as below.

```text
[<language>-e2e] start configs=<count> at=<iso-time>
[<language>-e2e] <Config> start scenario=<selector>
[<Config>] <Scenario> start
[<Config>] <Scenario> PASS (<seconds>s)
[<language>-e2e] <Config> PASS (<seconds>s)
[<language>-e2e] total PASS (<seconds>s)
```

`<language>` uses a short name distinguishing the language runner,
like `dotnet`, `java`, `node`, `cpp`. `<selector>` is `all`, a single
scenario ID, or a comma-separated scenario list the per-language
runner accepts. A runner like C++'s that repeats the same config
across several start orders also records `start_order=<variant>` on
the config start/complete lines.

On failure, `FAIL (<seconds>s, attempt <n>)` is printed at the same
spot. A failure that's a retry target, like a bind conflict, prints one
retry-notice line and then reruns the same config. Diagnostic output
like Redis startup logs, the log directory, and individual server
stdout/stderr can appear between these summary lines.

Each config runner must stream client scenario progress to the
console in real time. Writing only to a log file and showing it all at
the end looks like it's stuck, so that isn't treated as standard. If a
file log is needed, handle it in a way that satisfies both console
output and file storage together, like `tee`.

`Ctrl-C`, `TERM`, and normal exit all use the same cleanup path. On
exit, the aggregate runner only cleans up the current config's
sub-processes it started. Redis is cleaned up only for the container
id an individual config runner created itself. The cleanup function
must be safe to call multiple times. On interruption, print one line
announcing cleanup in progress, like below, then exit.

```text
[<language>-e2e] interrupted; stopping the current configuration...
```

### 2.2 Per-Language Porting Unit

When adding e2e to another language, implement one config not as a
small bundle of test files, but as an independently-runnable
deployment bundle. The default deliverables needed to implement one
config are as follows.

- `Shared/`: keeps only the request/reply/event/evidence DTOs the
  server and client use together.
- `Server/<Role>/`: one runnable app per role distinguished in a real
  deployment, like provider, consumer, publisher, subscriber, play,
  session. A replica of the same role can launch the same project
  multiple times, but different roles are split into separate
  projects and folders.
- `Server/<Role>/Configuration/`: keeps that role's runtime options
  and argument parsing.
- `Server/<Role>/Endpoints/`: keeps HTTP endpoint mapping. Includes
  both the app endpoints the client calls and operational endpoints
  like evidence/wait/shutdown.
- `Server/<Role>/Handlers/`: keeps types registered with the
  framework runtime, like a framework handler, route handler,
  observer.
- `Server/<Role>/Infrastructure/`: keeps implementation used together
  by endpoints and handlers but not part of the public message
  contract, like an evidence store or a role-internal state store.
- `Client/Program.*`: declares the list of scenarios to run, and calls
  either all or a single scenario in order depending on options.
- `Client/Scenarios/<ScenarioId><Name>Scenario.*`: one file per
  scenario ID.
- `Client/Support/`: keeps only auxiliary code shared across scenarios
  — option parsing, assertion, process manager, evidence wait.
- `run_e2e.*`: handles build, port allocation, log directory creation,
  server process start/stop, client execution, and printing log
  location on failure.
- `feature-map.ko.md`: records implemented scenarios, unimplemented
  scenarios, public-contract gaps, and harness gaps against the
  config document's scenario IDs.

Per-language file extensions or project file names can naturally
change. But if the role boundary and file classification above change,
per-language e2e results can't be compared with each other, so it
isn't treated as complete.

### 2.3 Role Server And Endpoint Shape

A role server is a small version of the app a user deploys. The
endpoints a client calls are also decided from this viewpoint.

- An endpoint represents an action a real user actually triggers.
  Example: publish trigger, request submit, subscribe/bind, admin
  `Relocate`/`Shutdown`, socket-weight-load exclude/restore, evidence
  wait, topology wait.
- One endpoint must not run several scenarios internally and just
  return the result. An endpoint that delegates client verification to
  the server, like `/run`, `/scenario/all`, `/execute`, is prohibited.
- Inside an endpoint, use that language's framework public API. Don't
  use private APIs, raw frame manipulation, reflection bypasses, or
  test-only adapters.
- An evidence endpoint exposes a marker the role server actually
  processed. Don't judge success only from a marker a scenario-
  execution-only server made.
- If waiting for a value to change is needed, put a bounded-wait
  endpoint on the role server. Example: `/evidence/wait`,
  `/topology/wait`, `/admin/weight/wait`. Don't use a method where the
  client repeats the same GET dozens of times to observe a value
  change.
- For a flow the server can push — stream, subscription, monitoring
  event — connect the client stream connector first and verify with
  the push payload. HTTP is only used as a trigger causing a state
  change.
- However, a config like Pub/Sub fanout, where the event's recipient
  is a subscriber role server rather than the client, uses the
  subscriber server's bounded evidence wait. Don't bypass the
  subscriber role by adding a separate observer via the client stream
  connector.

### 2.4 Server Project Structure Rules

- If server roles differ, place them as separate runnable projects
  under `Server/<Role>/`. Don't change one server project between
  publisher/subscriber, or normal/error/peer server, via a `--role`
  or `--mode` option.
- Keep only that role's code inside its per-role project. For example,
  a `Server/Provider` project must not have
  publisher/subscriber/play/session/multi-node branching and handlers
  all mixed together. Also prohibited: copying the same `Program.cs`
  into several role projects and just changing the default role.
- `<Role>` in `Server/<Role>/` must be a role that has meaning in a
  real deployment. Even if renamed to `Main`, `Coordinator`,
  `Control`, `Scenario`, it can't be built if that server only runs
  the scenario for you and provides no real feature.
- `Program.cs` keeps only the run entry point. Host configuration, DI
  registration, and framework setup go into `*HostFactory.cs`.
- Write `AddZLinkFramework` configuration and Store registration so
  they're directly visible in `*HostFactory.cs`. Register Location
  capability with `AddLocationStore(instance)`, and Relocation
  capability with `AddRelocationStore(instance)`, each separately.
  Don't use a Redis-only registration function or one that bundles both
  capabilities
  ([05 §10](../spec/06-framework-api.en.md#10-location-store-and-relocation-store)).
  Don't hide framework configuration behind a thin wrapper/extension
  method.
- Don't create separate runnable projects like `Server/Driver`,
  `Server/TestRunner`, `Server/ScenarioRunner`. Even under a different
  folder name, a server that's only delegated scenario execution is
  the same prohibited target. If process start/stop is needed to
  progress a test, handle it in `run_e2e.*` and client support code,
  and put the framework feature call inside the actual role server
  endpoint.
- The e2e server is a small runnable example, but keep folders per
  file character so other languages can follow the same pattern
  easily. Keep folders like `Configuration/`, `Endpoints/`,
  `Handlers/`, `Infrastructure/`. Even if there's only one file now,
  place it in that folder if its role is clear. This lets you
  immediately find "option parsing," "HTTP surface," "framework
  handler," "evidence storage" when comparing per-language
  implementations.
- Don't mix files of the same character between the project root and
  a subfolder. For example, avoid a mixed structure where some
  endpoints live in `Program.*` and some in `Endpoints/`.
- Don't put endpoints, handlers, or lengthy framework configuration
  into `Program.*`. Keep the entry point at the level of
  `RoleHostFactory.Create(args).Run()`, and split the actual host
  configuration into `*HostFactory.*`.
- Don't create a common server library/shared project by default. Even
  if it creates a bit of duplication, prefer each server project
  directly exposing its own configuration. Only consider a separate
  shared project when the same code truly repeats across many configs
  or roles and the maintenance cost grows large.
- `Shared/` keeps only message/contract types shared by server and
  client. Don't put server-only host factory, handler, filter, or
  evidence store into a config's top-level `Shared/`.

### 2.5 Client Project Structure Rules

- The client runs scenarios sequentially from `Client/Program.cs`.
- `Client/Program.cs` keeps only option parsing, HTTP client creation,
  and the scenario call order. Don't put an individual scenario's
  request/verification body in `Program.cs`.
- Split scenarios into per-scenario files under `Client/Scenarios/`.
- Describe at the top of each scenario file what that scenario
  verifies.
- Client support code only keeps things that assist the scenario flow,
  like option parsing, assertion, process lifecycle. Don't build a
  helper that hides a framework call so the client bypasses the server
  app.
- Generic assertion helpers like condition checking and expected-
  error/timeout verification are owned by `Client/Support`. Don't add
  the same helper to the connector package's production public API.
- The client calls the server app endpoint with the per-language HTTP
  client wrapper. A server evidence endpoint and log marker can be
  used for verification, but don't read framework internal state or a
  private/test-only API directly.
- A scenario waiting for a value change/event/push uses the client
  stream connector instead of a polling-only HTTP loop. The HTTP
  endpoint represents an action a user actually triggers — bind,
  subscribe, state-change trigger — and whether the change arrived is
  asserted by the connector's push receipt. For a language with no
  connector, or a feature with no public contract yet, leave it as a
  gap in the feature-map.
- The Pub/Sub fanout scenario is an exception. Since the subscriber
  role server is the actual recipient for that config, confirm
  subscriber handler evidence via a bounded-wait endpoint. This
  exception only applies to the subscriber role server's actual
  dispatch marker, not to evidence from a scenario-execution-only
  driver.
- A client scenario must directly show the actual role server
  endpoint call and verification flow. It must not just call a
  driver's single `/run` endpoint and let "the rest be verified on the
  server side somehow." Evidence lookup must also be fetched from the
  actual role server, not just read from a scenario-execution-only
  server's result.
- If there are several scenarios, split into several client scenario
  files too. Don't bundle several scenarios into one file — like
  `AllScenario`, `ScenarioSet`, `DriverScenario` — and delegate to a
  driver.
- One scenario ID in the config document must correspond to one client
  scenario file. For example, `RC-B1`, `RC-B2`, `RC-B3`, `RC-B4` are
  each kept as independent files, not one bundled codec file. Even if a
  common endpoint returns the same response, the client verification
  unit must be split.
- Multiple scenarios calling the same server endpoint is fine. But
  each scenario file only directly asserts the reply, push, topology,
  and evidence conditions its own ID must check.

### 2.6 Configuration Delivery Contract

E2E in every language mandatorily follows the
[Sample/E2E Configuration Policy](../sample-e2e-configuration-policy.en.md).
Each config runner generates a per-run role configuration file and
passes only the configuration file path to the framework host. A
standalone client that isn't a framework host receives the endpoint it
connects to directly, the request timeout, and the scenario selector
as explicit CLI options, validated once at startup. Endpoint, Redis,
routing id, timeout, log and evidence paths aren't passed via
environment variables or JVM system properties, and there are zero
environment variables directly usable by server and client application
code.

Scenario selector, process restart, and fault-injection commands are
E2E run-control inputs, so they're distinguished from configuration
values. The scenario selector is passed to the standalone client.
Process restart and fault injection are handled via a runner option or
a client support process-manager command, not passed to the framework
host's CLI.

Don't also provide an environment-variable interface as a fallback
path. The framework host uses a configuration file and typed binding,
and the standalone client uses validated CLI input or, if needed, a
typed configuration file. A lane that doesn't satisfy this contract is
recorded in the feature-map as a configuration gap.

### 2.7 `run_e2e.*` Execution Contract

Every language's run script must have the same usage meaning and the
same Redis launch method.

**Mandatory isolation rule:** every E2E run that needs Redis must
create a fresh dedicated Docker Redis container used only by that run.
Don't share or fall back to an already-running container, a host
Redis, or a Redis endpoint another E2E or sample made. Specifying only
a different key prefix doesn't permit instance sharing either. This
rule's purpose is to keep pause, stop, restart, delay injection, and
cleanup from affecting a different run.

The reference templates live under this directory's
`runner-templates/`.

- `runner-templates/redis-common.template.sh`: Redis helper reference
- `runner-templates/run_e2e.template.sh`: per-config e2e runner
  reference
- `runner-templates/run_e2e_all.template.sh`: aggregate e2e runner
  reference

- The default run executes that config's implemented scenarios in
  sequence.
- An individual config runner supports running a single scenario or a
  scenario list. Example: `./run_e2e.sh RC-B2`, `./run_e2e.sh RL-A4`,
  `./run_e2e.sh RL-A4,RL-C2`, `./run_e2e.sh RL-A4 RL-C2`. Comma and
  space arguments mean the same thing, and the runner normalizes them
  into one scenario selector the client understands before passing it
  along.
- The script is responsible for the order: build → create log
  directory → start server → confirm readiness → run client → stop
  server.
- Each language keeps a Redis helper shared by its e2e runners. The
  helper provides per-run Redis container startup and cleanup of the
  container id that run created as common functions, so individual
  config scripts don't assemble Docker commands directly.
- Readiness isn't judged by a fixed sleep alone. Confirm via each role
  server's `/health`, port open, or an explicit marker.
- On failure, print `log_dir=...` and leave each role server's and
  client's stdout/stderr/framework log.
- Scenario selection is done by the client, but the client doesn't
  delegate the whole run to a server-side scenario runner.
- Cases needing process control — scale-out, restart, crash, store
  outage — are handled by the script or client support process
  manager. The framework request/send/publish itself is only performed
  inside the actual role server endpoint.
- A config's individual `run_e2e.*` that needs a Redis location store
  starts a fresh dedicated Docker Redis container per run. Don't reuse
  an already-up Redis container or a host Redis endpoint. Even with a
  different Redis key prefix, fault injection, pause/stop/restart,
  flush, cleanup, and latency injection can affect a different run. On
  exit, clean up only the container id it created itself. An
  individual script must not remove a different Redis container with
  the same prefix.
- If Docker Redis can't be created, the runner fails immediately.
  Don't auto-fall-back to a host Redis or another run's endpoint and
  treat it as success.
- Redis container startup uses the same order in every language.
  Create the container with
  `docker create --name <scoped-name> --tmpfs /data -p 127.0.0.1::6379 <pinned-redis-image>`,
  start it with `docker start <container-id>`, then read the run state
  and assigned host port with `docker inspect`. Don't rely on `docker
  run -d` output to handle container id and port at once.
- E2E Redis data is only needed during the run, so don't create a
  Docker volume. Override the `/data` volume the Redis image declares
  with `--tmpfs /data`, and use `docker rm -fv` for container cleanup.
  This keeps an anonymous volume from being left behind after repeated
  runs.
- Add a prefix to the Redis container name revealing the language and
  the e2e run scope. For example, Java e2e uses
  `zlink-redis-java-e2e...`, Kotlin e2e uses
  `zlink-redis-kotlin-e2e...`. Other languages must also be able to
  identify the `<language>-e2e` scope from the name with the same
  rule.
- The aggregate e2e runner doesn't clean up another run's Redis and
  calls each config's individual `run_e2e.*` in sequence. Configs
  aren't run in parallel within one aggregate run, but it must not
  share or remove resources with a different individual run of the
  same language, or with another aggregate run.
- The aggregate e2e runner must also be able to narrow the run target.
  With no argument, it runs `all` for every config; with an argument,
  it runs only the specified configs. To run only some scenarios
  within a config, use the `Config:ScenarioA,ScenarioB` form. Example:
  `./run_e2e_all.sh RegistrationCodec:RC-B2,RC-B4` or
  `./run_e2e_all.sh ResilienceLifecycle:RL-A4,RL-C2 PubSub:PS-A1`.
  The aggregate runner only interprets this selection info; actual
  readiness, Redis endpoint creation, server startup, and client
  scenario execution are delegated to that config's individual
  `run_e2e.*`.
- The aggregate e2e runner doesn't reimplement per-config internal
  behavior. It calls the selected individual `run_e2e.*` and only
  manages retry and final result. Redis endpoint creation, readiness,
  log location, and scenario-execution detail are owned by the
  individual config script and common helpers.
- The Redis host port isn't fixed. Let Docker assign a free loopback
  port, and have the runner get the endpoint from the inspect result
  and pass it to each role server and client. Redis key prefix,
  routing id, and log directory must also be unique per run.
- Even if a different sample/e2e is using Redis on the same host, its
  endpoint isn't borrowed. Creating a new Docker Redis container and
  using a different loopback port Docker assigns is what prevents
  test interference.
- Wrap the Docker command itself with a short timeout, and confirm
  Redis readiness separately with a port/readiness wait function.
  Don't handle a slow Redis start and an unresponsive Docker CLI with
  the same sleep.
- If the Redis helper fails, the individual runner must also fail
  immediately. In a shell runner, don't receive the container id by
  reading a process-substitution result like
  `read ... < <(redis_start_function)`. This approach can let `read`
  itself succeed even if the helper fails, leading to the wrong
  outcome of starting the server without Redis. Provide the helper as
  a function that assigns a value into the caller's variable, like
  `zlink_redis_start_scoped_assign`, so the function's failure directly
  becomes the runner's failure.
- The aggregate e2e runner may retry only a limited set of transient
  bind failures (`Address already in use`, `EADDRINUSE`,
  `already bound`, `errno=98`). A scenario assertion failure, a
  runtime semantic failure, a native abort, and an unmet store-recovery
  condition aren't retry targets — leave the cause log and fail.

### 2.8 feature-map Writing Rules

Per-language e2e keeps a per-config `feature-map.ko.md`. This document
isn't a skip list — it's a table recording implementation status and
the basis for gaps.

- Put every scenario ID from the config document as a row.
- State status clearly, like `implemented`, `not-supported`,
  `blocked`, `deferred`.
- `not-supported` means the feature doesn't exist in that language's
  public contract. In this case, also write whether there's a needed
  public API and related spec/guide basis.
- `blocked` is used only when there's an underlying cause to fix, like
  a runtime bug, bindings bug, or lack of harness. Don't weaken a
  scenario to dodge a bug.
- Even with an unimplemented item, if it's P0 it's not complete. P1/P2
  write feature-support status and run cost together.

### 2.9 Comment Writing Rules

- At the top of a scenario file, write what user flow and what
  framework behavior this file verifies. A reader must immediately
  know "why this scenario is needed" upon opening the file.
- A comment is only used to explain judgment that isn't revealed by
  code alone — scenario intent, verification criteria, why a wait is
  needed. Don't add a comment that just repeats what the code does.
- A short comment can be placed at a scenario's core step — an HTTP
  call, a server evidence lookup, a process restart. This comment
  should explain "what real-usage condition this step creates" rather
  than "what is being called."
- Don't substitute a helper or support code comment for explaining the
  core flow. Reading only the scenario body should show what request
  is sent and what is checked via the connector or HTTP client.
- Don't make an item that couldn't be implemented for lack of public
  contract look "supported" via a comment. Leave that item in the
  feature-map or an issue, and only write the currently-verified
  public behavior in the comment.

## 3. Config List

Each config takes one realistic server configuration as a unit and
verifies detailed behavior — messaging, connection, spot, codec, etc.
— on top of it.

| Config | Server configuration | What it covers |
|--------|-----------|-----------|
| [Config 1 — Location messaging](config-1-location-messaging.en.md) | Location Store + 2 Channel providers + 2 Object Servers + 2 Object Clients | Public RouteMesh status, automatic/manual topology, Channel provider selection, request/send, timeout/backpressure, and global object identity conflict |
| [Config 2 — Spot service](config-2-spot-service.en.md) | Location Store + Relocation Store + 2 Play nodes + 2 Session gateways | Entry/User Spot, Actor create/Join, direct/Channel/multicast message, Session bind/relay/push, timer/close, crash and scale-out |
| [Config 3 — Pub/Sub events](config-3-pubsub.en.md) | Publisher + 3 subscribers + Location Store | Publisher discovery, topic filter, fanout, late subscriber, restart/Store failure, and publish semantics that don't replay |
| [Config 4 — Registration/codec](config-4-registration-codec.en.md) | Channel caller/provider | Per-language handler registration method, startup validation, DI lifecycle, default typed JSON and root codec extension |
| [Config 5 — Resilience/lifecycle](config-5-resilience-lifecycle.en.md) | Multiple nodes + Location Store | Restart/replacement/disconnect, terminal-once, hidden-replay prohibition, Relocate/Shutdown, capacity and lifecycle contention |
| [Config 6 — Store failure/recovery](config-6-store-failure-recovery.en.md) | Location/Relocation Store + 2 providers + consumer | Public failure during store failure, owner invalidation and recovery, relocation result, user-observed result of capacity reservation |
| [Config 7 — Monitoring](config-7-monitoring.en.md) | Location Store + 2 services | Public RouteMesh/host status, topology change, store-failure reflection, slow-observer isolation, and bounded snapshot |
| [Config 8 — Execution turn](config-8-execution-turn.en.md) | 2 Play nodes + 2 worker services + gateway | Spot/Actor serial execution, Yield/worker, deferred operation, timeout/cancellation/shutdown, and per-language parity |
| [Config 9 — To-actor messaging](config-9-to-actor-messaging.en.md) | 2 Actor nodes + 2 Session gateways + caller | Actor-ID send/request independent of binding, Actor recreation, public result of stale location and route failure |
| [Config 10 — Spot actor join/relocation](config-10-spot-actor-relocation.en.md) | Location/Relocation Store + 2 Actor nodes + 2 Session gateways + caller | Local/remote Join, state and message order during a move, Session-binding route refresh, Message Follow, PerActor/SpotWide relocation |
| [Config 11 — Observability/operational deployment](config-11-observability-ops.en.md) | Session + 2 Play + 2 workflow + Stores | Public flow correlation/metrics, maintenance Relocate/Shutdown, client/application result of patch and drain |
| [Config 12 — Channel egress routing](config-12-channel-egress-routing.en.md) | Session/Play/API + 2 ClientServer services | ChannelName routing, local-egress selection, weight/shutdown/restart, and request/send terminal |
| [Config 13 — One-way submit admission](config-13-submit-admission.en.md) | RouteMesh/ClientServer/Spot/Actor/Stream targets | One-way admission completion, timeout/cancellation/shutdown contention, zero target, ordering, and hidden-retry prohibition |
| [Config 14 — Instance Spot activation](config-14-instance-spot.en.md) | Location/Relocation Store + 2 callers + 2 owners + User Spot owner | Cold activation, concurrent first call, first-message ordering, crash/deadline/capacity/relocation, and cross-language result |

## 3.1 Configuration Axes — Variations That Run Across Configs

Even the same scenario can have a defect that only shows up in a
specific combination of server configuration. Verifying the
consolidation of a previous topology into a single MeshNode found
defects in several languages' frameworks, and all of them were "the
feature itself had an e2e, but no one had ever run that combination of
configuration" paths. To prevent this, a config's core scenario is
also verified as a variation along the following axes.

| Axis | Variation | Mainly hits which config | Actual case found |
|----|------|--------------------|----------------|
| MeshNode configuration | Configure ChannelName/Spot/Actor together with a single RouteMesh and location descriptor | Config 2, 9 | An implementation where remote actor join assumed a separate channel or Spot transport |
| Placement | Split session and spot into **different processes** (paired with the colocated variant) | Config 2, 9 | With colocated, the local join means the remote relay path doesn't even execute |
| rid direction | A variant reversing the rid lexical order so the requester becomes the auto-connect **non-initiator** | Config 1, 2, 9 | A missing spot response correlation on the non-initiator (recv pump) — reproducible only by reversing rid |
| peer count | One sender making **consecutive requests to 2+ nodes** | Config 1, 2 | A missing response drain on the second peer |
| startup order | A variant that **reverses the startup order** of server roles (dependency-reversed startup) | Config 1, 2, 9 | A connection-convergence race that only shows up in a specific startup order — not reproducible in a runner with a fixed order |

An axis variation isn't writing a new scenario — it's rerunning the
same client scenario with only the server topology changed. Not every
combination needs to run; apply the "no route mesh × split placement"
combination first to each config's P0 scenario (the majority of found
defects came from this combination).

### Startup-Order Axis Procedure

A startup-order defect doesn't reproduce in a runner with a fixed
order, so the runner must be able to run with the order changed.

- The config runner takes the server-role startup order as an argument
  (e.g. `E2E_START_ORDER=reverse|shuffle:<seed>`). The default is
  forward (dependency order), and the axis variation runs the minimum
  scope of **one full reverse pass + one fixed-seed shuffle pass.**
  Record the seed so a failing combination is reproducible.
- **The result must be the same regardless of which order it comes up
  in**: even if each role's dependency (store, peer) comes up late,
  discovery/connection must converge, and the first request right
  after convergence succeeds without retry. If it works or doesn't
  work depending on the order, that itself is a defect.
- Client startup is fixed to after every role's readiness — what this
  axis verifies is order-independence of inter-server mutual
  discovery, not the client's early connection attempt (that's covered
  separately by the "first request right after convergence"
  requirement).

### Verification Requirements Every Config Must Meet Separately From The Axes

- **Contract round-trip**: assert that a value is preserved when a
  framework public type (routing id, actor ref snapshot, etc.) crosses
  the channel/spot/stream surface. An actor ref carried in a response
  must be concrete (node rid not empty, generation > 0). — A missing
  serialization is visible in the sending-side log's value, so it
  isn't caught without asserting the received value.
- **No silent drop**: a send/request addressed to an unregistered
  handler must not be silently dropped — it must leave an observable
  failure (an error response or a log marker). A type that only shows
  up as an unanswered timeout has the highest diagnostic cost.
- **Ownership consistency**: explicitly run a combination where the
  request that creates state (start) and a subsequent request
  (continue) go to different nodes, and check whether an ownership
  violation is classified as fail-fast and whether owner-consistent
  routing prevents it. If ownership is hash-based, the failure is
  intermittent, so increase repetition count (there's a case where 3
  consecutive passes wasn't enough).
- **First request right after convergence**: send the first request
  immediately after location discovery/dial convergence, with no
  settle delay. Don't mask it with a retry or sleep — whether the
  first request succeeds right away or is classified fail-fast is
  itself the target of verification.
- **Infrastructure gate**: for a config where the location store is
  mandatory, a build/configuration without a store must fail at
  configuration time, not run silently disconnected. The runner
  script only uses standard tools (there's a case where depending on
  an uninstalled tool invalidated the entire judgment loop).
- **Multi-stage push chain**: assert all the way through a chain that
  crosses a role boundary two or more times inside the server —
  channel request → actor send → bound session push — and finally
  reaches the client stream. A missing registration/connection at an
  intermediate hop isn't revealed just by the sending role's success
  log — whether the client actually received the push is the only
  success criterion.

## 4. Priority

| Priority | Meaning | Implementation standard |
|----------|------|-----------|
| `P0` | Verification that must exist to claim a config's core feature | Implemented in every language |
| `P1` | Verification a language documented as supporting a specific feature must pass | Implemented in supporting languages |
| `P2` | High-cost verification like operational scale, rolling update | Optionally applied to the release gate, with the reason for non-implementation recorded |

Priority of axis variations (§3.1): "no route mesh × split placement"
is applied as `P0` to Config 2/9's P0 scenarios, and the rid-direction
and multi-peer variants are applied as `P1`.

## 5. Common Run Principles

- A test uses an independent temporary working directory and log
  directory.
- A server process starts per the role the config declares. A config
  needing a shared location store either prepares the store (Redis,
  etc.) before running or starts it as a separate process, and cleans
  up the store process or container it created afterward. A
  multi-process config's shared store defaults to the official Redis
  extension. If a single-process smoke needs location lookup, a
  process-local `IZLinkLocationStore` implementation can be registered
  with `AddLocationStore(instance)`.
- Port, routing id, Redis key prefix, and store path are isolated per
  run. A runner using Docker Redis creates the container with a
  language/e2e-scope prefix and cleans up only the container id it
  created itself. The aggregate runner doesn't remove a different
  run's container with the same prefix either.
- Server readiness isn't judged by sleep alone — confirm via port
  readiness or a readiness marker.
- The success criterion combines the client return value, the push the
  client stream connector received, and the server's public
  application evidence with the formal public flow/metric record. A
  general diagnostic log is investigation material for failure, not a
  success condition. A config using the Location Store also adds
  public RouteMesh status and the Actor/Spot manager's resolve result
  to the success criteria. Application and E2E client don't directly
  read or interpret the Store provider record.
- On failure, leave each process's stdout/stderr, framework log, and
  the client's last request info.
- On failure, first separate the cause layer. Judge with evidence
  which of `core-capi`, `bindings`, `framework`, `sample`, or the test
  run script it is, and put a regression test on the fixed layer.
  Don't work around a C API or bindings bug inside framework just to
  pass a framework test.
- The same scenario must only differ in per-language public API shape
  — the meaning and markers must be the same.

## 6. Logging And Message Flow Tracing (Required, Common)

Every e2e **must turn on file logging**, and also turns on message
flow tracing when authoring/debugging a normal scenario. Don't
substitute ad-hoc `printf` or console scrolling. Since tracing records
"did the message arrive / did it go to the handler / did the response
go out" as a standard feature, use it as the test's primary debugging
tool. A scenario verifying tracing's `off` behavior and a runtime
level change turns off tracing only for that span.
(Feature spec: [Message Flow Tracing And Dispatch Observation](../spec/26-message-flow-tracing.en.md))

### 6.1 All Logs To File (`log/` Folder)

- Each server/host and client process outputs every framework log as
  **a file under a per-run `log/` folder.** Don't stop at console
  output alone.
- The log directory is isolated per run (§5) and excluded from VCS
  (`.gitignore`). (C++ Bingo example: `samples/Bingo/logs/`,
  `run_sample.sh` exports `BINGO_LOG_DIR`.)
- The file sink uses an API that auto-creates the parent directory
  (C++ `app.logging().use_file(...)`/`use_rotating_file(...)`;
  `.NET`/Java/Node use equivalent-meaning options too). It must not
  silently fail just because the directory doesn't exist.
- Split into a file per process (e.g. `provider-a.log`, `play-a.log`,
  `session-a.log`, `client.log`) so which node's log it is is
  immediately visible.

### 6.2 Turning On Message Flow Tracing (Primary Debugging Tool)

- Turn on message flow mode at **at least `key_transitions`** during
  an e2e run. Then one message's inbound
  (`received`→`dispatched`/`replied`) and outbound
  (`sent`→`reply_received`) are each printed as one line. A failure
  (`dropped`/error) is printed on the same stream with the same
  `corr=`, so success and failure read as one flow.
- Log-line tokens: `zlink flow: phase=… surface=… kind=… packet=…
  channel=… topic=… corr=… src=… spot=… actor=… [size=]`.
- **Grep by `corr=<id>` to trace one request's lifecycle.** Across
  nodes, `corr` continues through the path it propagates through
  (channel request↔reply, stream request↔reply echo, route
  propagation). (Note: `corr` is a per-process global monotonic value,
  so each node's counter is independent — the number alone matching
  can be a different message, so trust an inter-node link only on a
  path where `corr` is actually propagated. The spot subscription/
  actor/publish path is keyed by spot/actor id instead of `corr`.)
- Tracing logs can be merged into one file with app logs (via the app
  logger sink) or split into a dedicated file (C++
  `diagnostics.log_file`). Either way, **leave them as a file** per
  §6.1.
- A runtime level change is performed via public runtime control. A
  normal e2e capture keeps at least `key_transitions`, and Config 11
  `OBS-A5` confirms trace-dedicated flow info and log messages aren't
  produced after switching to `off`, then turns it back on.
- Tracing is **observation, not control.** Turning it on must not
  change feature behavior or the success criterion, and an
  observer/trace failure must not change message processing or test
  judgment.

### 6.3 Included In Failure Evidence

- On scenario failure, leave the above **file log (including flow
  tracing)** as evidence, in addition to §5's stdout/stderr/client
  info. Narrow the cause-layer separation
  (`core-capi`/`bindings`/`framework`/`sample`/test run script) via
  the `corr=` flow first too.

### 6.4 Per-Language Application Standard

File logging (§6.1) is mandatory in every language. A language
supporting message flow tracing must also leave §6.2's evidence. A
language that hasn't yet implemented the tracing contract records the
missing evidence and applicability condition in the feature map, and
doesn't substitute a different log for tracing evidence.

## 7. Scenario ID Rule

An ID uses `config prefix - track - number`. Example: `RM-A1`
(Location messaging, Track A, number 1). Config 1 uses the `RM`
prefix.

| Prefix | config |
|--------|--------|
| `RM` | Location messaging |
| `SM` | Spot messaging |
| `PS` | Pub/Sub |
| `RC` | Registration/codec |
| `RL` | Resilience/lifecycle |
| `SF` | Store failure/recovery |
| `MON` | Monitoring |
| `TD` | Execution turn and terminator |
| `TA` | To-actor messaging |
| `ST` | Spot/Actor relocation |
| `OBS` | Observability and operational control |
| `CH` | Channel egress routing |
| `SA` | One-way submit admission |
| `IS` | Instance Spot |

The test name can be changed to match language convention, but the
config id and scenario id must be visible in the report.

## 8. Completion Criteria

- Every config's `P0` scenario must be implemented.
- `P1` must be implemented in a language documented as supporting that
  feature.
- An unsupported feature must have a reason in the feature map and
  documentation, not just be a skipped test.
- The failure path is verified with both the result the client
  received and the log or evidence the actual role server left.
- A test with a workaround added while cause classification is still
  unfinished isn't treated as complete.
