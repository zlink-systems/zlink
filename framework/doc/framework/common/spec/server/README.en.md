# Framework Common Spec

The documents in this directory describe the Framework's common public
contract. Each document self-contains the inputs, state, normal flow, and
failure/completion conditions its implementation and contract tests need.

This directory and the per-language interfaces are the single authority
for the Framework public contract. The documents in this directory form two
layers (the "Layer" column in the topic tables below). The **contract** layer
defines the behavior the application observes, and the **implementation spec**
layer defines the structural decisions every language's service runtime
follows in common so that it delivers that contract with the same result. A
single document can carry both layers, in which case each sentence states
which one it is. The implementation spec adds no new public behavior, but it
is normative for runtimes — breaking a decision changes what the application
observes. A conflict between the two layers is a defect. The implementation
spec is corrected against the contract; if the contract itself must change,
the
[public contract procedure](00-foundation/01-public-contract-governance.en.md#5-public-contract-procedure)
is followed first.

The rule for isolating verification runners so that samples and E2E suites for
multiple language implementations of the same contract can run concurrently on
one host belongs to the verification environment, not to this spec — see the
[sample runner isolation standard](../../sample/README.en.md#the-sample-run-script-and-redis-isolation-standard)
and the
[E2E runner execution contract](../../e2e/README.en.md#27-run_e2e-execution-contract).

## What This Spec Answers

| Topic | Reader's Question | Entry Document |
|---|---|---|
| foundation | What rules does this spec as a whole follow, and what common vocabulary and API registration does it use | [00-foundation/README.en.md](00-foundation/README.en.md) |
| execution | When and in what order does a handler run, and what structure guarantees completion, cancellation, and concurrency | [01-execution/README.en.md](01-execution/README.en.md) |
| channel-transport | How are the physical connections between MeshNodes and the paths that send messages over a Channel structured | [02-channel-transport/README.en.md](02-channel-transport/README.en.md) |
| spot-actor | What are Spot and Actor, and what path does a message take to reach one | [03-spot-actor/README.en.md](03-spot-actor/README.en.md) |
| session | How is one external connection (a session) tied to an Actor, and what is guaranteed when it disconnects or moves | [04-session/README.en.md](04-session/README.en.md) |
| location-relocation | How is the current location of an Actor or Spot found, and what is preserved when it moves to another node | [05-location-relocation/README.en.md](05-location-relocation/README.en.md) |
| observability | What does an operator use to check the Framework's current state and the cause of a failure | [06-observability/README.en.md](06-observability/README.en.md) |

## Reading Order

**First-time reader** (new to this spec as a whole)

1. foundation
2. channel-transport
3. spot-actor
4. session
5. location-relocation
6. observability
7. execution — only when needed, to check implementation detail

**New-language porting owner** (implementing a new service runtime)

1. foundation
2. execution
3. channel-transport
4. spot-actor
5. session
6. location-relocation
7. observability

**Application developer** (using the Framework through an existing language binding)

1. foundation
2. channel-transport
3. spot-actor
4. session
5. observability
6. location-relocation — only when calling Host relocation directly
7. execution — usually not needed. Implementation detail is already reflected in the contract

## Topics

### 00-foundation

Covers the contract-ownership rules, vocabulary, top-level model, interaction
targets and completion semantics, message/response/error shapes,
language-neutral registration API, and runtime layering boundaries shared by
the whole Framework. Every other topic assumes this topic's vocabulary and
rules.

| Document | Question It Answers | Layer |
|---|---|---|
| [01. public-contract-governance](00-foundation/01-public-contract-governance.en.md) | What procedure must a change to the Framework public contract follow | Contract |
| [02. glossary](00-foundation/02-glossary.en.md) | What exactly does each term that recurs throughout this spec mean | Contract |
| [03. overview](00-foundation/03-overview.en.md) | What layer is the Framework, and what does each language implement separately | Contract |
| [04. interaction-model](00-foundation/04-interaction-model.en.md) | What is a Framework operation's target, and when is it considered complete | Contract |
| [05. message-model](00-foundation/05-message-model.en.md) | What shape and rules do a sent message and its response/error follow | Contract |
| [06. framework-api](00-foundation/06-framework-api.en.md) | What must an application register at the root to start the Framework | Contract |
| [07. framework-error-model](00-foundation/07-framework-error-model.en.md) | What common error does an application receive when Send/Request fails | Contract |
| [08. layering](00-foundation/08-layering.en.md) | What pieces does runtime code split into, and what values must never be merged | Implementation spec |

### 01-execution

Covers the full execution path from submit through handler execution,
completion, cancellation, execution serialization, and payload ownership —
everything from an accepted call to its arrival at and completion in the
handler. Most of it is implementation spec that every language's service
runtime must follow in common.

| Document | Question It Answers | Layer |
|---|---|---|
| [01. submit-and-completion](01-execution/01-submit-and-completion.en.md) | When is a call accepted, and what completes it | Contract+Implementation |
| [02. handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.en.md) | Why is state safe even though the handler has no synchronization code | Contract+Implementation |
| [03. cancellation-and-shutdown](01-execution/03-cancellation-and-shutdown.en.md) | How do cancellation and shutdown treat work already accepted | Contract |
| [10. spot-timer](03-spot-actor/10-spot-timer.en.md) | When does a Spot timer run, and what happens to a late tick | Contract+Implementation |
| [04. application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.en.md) | Under overload, what is blocked first, and what does the application observe | Contract+Implementation |
| [05. payload-ownership-and-codec](01-execution/05-payload-ownership-and-codec.en.md) | How many times is a message's bytes copied from the socket to the handler | Contract+Implementation |

The shared-permit rule carried over from the session topic is owned as a
single contract sentence by `05`'s "Ordinary ingress permit order" section.

### 02-channel-transport

Covers the physical connections (RouteMesh, ClientServer, listener identity),
how Node-direct and Channel select-one choose a target over them, connection
liveness checks, and the byte/command format on the wire.

| Document | Question It Answers | Layer |
|---|---|---|
| [01. channel-topology](02-channel-transport/01-channel-topology.en.md) | How are RouteMesh's physical connections and ChannelName's logical membership structured | Contract |
| [02. channel-messaging](02-channel-transport/02-channel-messaging.en.md) | How do Node-direct and ChannelName select-one each choose a target | Contract |
| [03. client-server-channel](02-channel-transport/03-client-server-channel.en.md) | How does a Server respond with a handler to a request a Client started | Contract |
| [04. network-listener-identity](02-channel-transport/04-network-listener-identity.en.md) | Why do a listener's bind address and advertised address differ, and when is each used | Contract |
| [05. transport-liveness](02-channel-transport/05-transport-liveness.en.md) | How is a remote connection's liveness checked, and how is it reconnected when it drops | Contract+Implementation |
| [06. wire-protocol](02-channel-transport/06-wire-protocol.en.md) | What bytes and commands actually pass between nodes | Implementation spec |

### 03-spot-actor

Covers the three Spot kinds (Entry, User, Instance) and Actor identity,
membership, and relocation, together with the two paths a message takes to
reach one (Spot-direct, Logical Multicast) and when the Location Store is
re-queried.

| Document | Question It Answers | Layer |
|---|---|---|
| [01. spot-model](03-spot-actor/01-spot-model.en.md) | When is each Entry/User/Instance Spot created, and what do they share and not share | Contract |
| [02. spot-messaging](03-spot-actor/02-spot-messaging.en.md) | What path does a message sent to a Spot take to reach the actual Spot | Contract |
| [03. mesh-node](03-spot-actor/03-mesh-node.en.md) | What is a MeshNode's identity, its object-placement conditions, and its startup order | Contract |
| [04. actor-model](03-spot-actor/04-actor-model.en.md) | How are an Actor's identity, location, message queue, and lifecycle defined | Contract |
| [05. spot-actor-membership](03-spot-actor/05-spot-actor-membership.en.md) | How is an Actor created, and in what order do Spot membership and relocation happen | Contract |
| [06. spot-address-messaging](03-spot-actor/06-spot-address-messaging.en.md) | How is a global SpotId created and looked up, and how is that Spot called directly | Contract |
| [07. stage-wrapper-on-spot](03-spot-actor/07-stage-wrapper-on-spot.en.md) | How is a higher-level execution model such as room or stage built on top of the Spot contract | Contract |
| [08. routing](03-spot-actor/08-routing.en.md) | When does a message to a Spot or Actor re-query location, and when not | Contract+Implementation |
| [09. object-lifecycle](03-spot-actor/09-object-lifecycle.en.md) | How does code distinguish the three Spot kinds, and when is a missing object created | Implementation spec |

### 04-session

Covers the registration, acceptance, codec, and error boundary of a single
STREAM connection (a session), and the Session's responsibility during
binding, rebinding, disconnect, and relocation of the connection to an Actor.

| Document | Question It Answers | Layer |
|---|---|---|
| [01. stream-session](04-session/01-stream-session.en.md) | Once a connection is accepted, what path does a packet take to reach the callback | Contract |
| [02. session-actor-binding](04-session/02-session-actor-binding.en.md) | How is a Session tied to an Actor, and what is guaranteed while the connection is being replaced or moved | Contract+Implementation |

### 05-location-relocation

Covers how the current location of an Actor or Spot is found (the Location
Store), how a request completing after relocation is recovered (the
Relocation Store), the common order for a planned move (Host relocation,
Actor Join, and so on), and the scope of automatic failover.

| Document | Question It Answers | Layer |
|---|---|---|
| [01. location-runtime](05-location-relocation/01-location-runtime.en.md) | How does the Framework find an object's current location and move it to another node | Contract |
| [02. location-store-redis](05-location-relocation/02-location-store-redis.en.md) | What must a direct implementation of the Location Store guarantee | Contract |
| [03. relocation-store-redis](05-location-relocation/03-relocation-store-redis.en.md) | What must a direct implementation of relocation-related payload storage guarantee | Contract |
| [04. relocation-flow](05-location-relocation/04-relocation-flow.en.md) | In what order do owner and message change while moving an Actor or Spot to another node | Contract+Implementation |
| [05. host-relocation-flow](05-location-relocation/05-host-relocation-flow.en.md) | In what order does Host `Relocate` move workloads, and what does `Shutdown` clean up | Contract |
| [06. failure-failover-policy](05-location-relocation/06-failure-failover-policy.en.md) | When a failure occurs, how far does the Framework automatically continue the same work | Contract |

### 06-observability

Covers how an operator queries the current state, aggregates values over
time, and traces the progress of a single message and a business flow chained
across several messages. The order for chasing an intermittent failure is
defined by
[README "4. The Order For Chasing An Intermittent Failure"](06-observability/README.en.md#4-the-order-for-chasing-an-intermittent-failure),
and the cost rule for leaving tracing on is defined by
[03. message-flow-tracing "5. Changing The Record Level At Runtime And The Cost Rule"](06-observability/03-message-flow-tracing.en.md#5-changing-the-record-level-at-runtime-and-the-cost-rule).

| Document | Question It Answers | Layer |
|---|---|---|
| [01. runtime-monitoring](06-observability/01-runtime-monitoring.en.md) | How does an operator query the Framework runtime's current state and find a cause in the log | Contract |
| [02. runtime-metrics](06-observability/02-runtime-metrics.en.md) | What are the names, units, and labels of the metrics for throughput, waiting, and failure | Contract |
| [03. message-flow-tracing](06-observability/03-message-flow-tracing.en.md) | How does one confirm how far a single message got and where it failed | Contract |
| [04. flow-correlation](06-observability/04-flow-correlation.en.md) | How is a request and its reply, or a business flow chained across several messages, identified | Contract |

## Per-Language Interfaces

The public types, signatures, and asynchronous representation each
language uses for the common server contract are owned by the following
documents.

- [C++](languages/cpp/README.en.md)
- [.NET](languages/dotnet/README.en.md)
- [Java](languages/java/README.en.md)
- [Kotlin](languages/kotlin/README.en.md)
- [Node.js](languages/node/README.en.md)

## HTTP Client

- [HTTP client spec table of contents](../http-client/README.en.md)
- [12 HTTP client integration contract](../http-client/12-http-client.en.md)
- [Per-language HTTP client contract](../http-client/language-interfaces.en.md)

## Stream Connector

- [32 Stream connector](../stream-connector/32-stream-connector.en.md)
- [Per-language Stream connector contract](../stream-connector/README.en.md#per-language-public-api)

## Citation Convention

A citation uses the **section title**. Clicking the link jumps straight to
that section.

```markdown
[Actor Model "3. Actor Queue"](03-spot-actor/04-actor-model.en.md#3-actor-queue)
```

**Do not cite by line number.** A `§123` form only lands at the top of the
document, forcing the reader to search again, and it goes stale the moment
the cited document changes by even one line. A section title breaks only when
that section disappears or is renamed, and link checking catches that.

The anchor is the title lowercased with spaces joined by `-`. Verify with:

```bash
mkdocs build --strict   # run from doc/site
```

## Where Old Documents Went

This spec reorganized the old layout, where every document carried one global
number (`00` through `52`), into topic directories. A link or memory keyed on
an old number finds the new location in the table below. Where an old
document split across several new documents, the section ranges are given.

| Old Document | New Location |
|---|---|
| `00-public-contract-governance` | [00-foundation/01-public-contract-governance](00-foundation/01-public-contract-governance.en.md) |
| `01-glossary` | [00-foundation/02-glossary](00-foundation/02-glossary.en.md) |
| `02-overview` | [00-foundation/03-overview](00-foundation/03-overview.en.md) |
| `03-interaction-model` | [00-foundation/04-interaction-model](00-foundation/04-interaction-model.en.md) |
| `04-message-model` | [00-foundation/05-message-model](00-foundation/05-message-model.en.md) |
| `05-async-execution-policy` | §1.1–§1.4·§2·§6 → [01-execution/01-submit-and-completion](01-execution/01-submit-and-completion.en.md) · §1.1(Yield)·§3·§3.1 → [02-handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.en.md) · §4 → [03-cancellation-and-shutdown](01-execution/03-cancellation-and-shutdown.en.md) · §5 → [04-spot-timer](03-spot-actor/10-spot-timer.en.md) · §10 → [05-application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.en.md) |
| `06-framework-api` | [00-foundation/06-framework-api](00-foundation/06-framework-api.en.md) |
| `07-channel-topology` | [02-channel-transport/01-channel-topology](02-channel-transport/01-channel-topology.en.md) |
| `08-channel-messaging` | [02-channel-transport/02-channel-messaging](02-channel-transport/02-channel-messaging.en.md) |
| `09-client-server-channel` | [02-channel-transport/03-client-server-channel](02-channel-transport/03-client-server-channel.en.md) |
| `10-network-listener-identity` | [02-channel-transport/04-network-listener-identity](02-channel-transport/04-network-listener-identity.en.md) |
| `11-spot-model` | [03-spot-actor/01-spot-model](03-spot-actor/01-spot-model.en.md) |
| `12-spot-messaging` | [03-spot-actor/02-spot-messaging](03-spot-actor/02-spot-messaging.en.md) |
| `13-mesh-node` | [03-spot-actor/03-mesh-node](03-spot-actor/03-mesh-node.en.md) |
| `14-actor-model` | [03-spot-actor/04-actor-model](03-spot-actor/04-actor-model.en.md) |
| `15-spot-actor` | [03-spot-actor/05-spot-actor-membership](03-spot-actor/05-spot-actor-membership.en.md) |
| `16-spot-address-messaging` | [03-spot-actor/06-spot-address-messaging](03-spot-actor/06-spot-address-messaging.en.md) |
| `17-stage-wrapper-on-spot` | [03-spot-actor/07-stage-wrapper-on-spot](03-spot-actor/07-stage-wrapper-on-spot.en.md) |
| `18-object-routing` | [03-spot-actor/08-routing](03-spot-actor/08-routing.en.md) |
| `19-stream-session` | [session/01-stream-session](04-session/01-stream-session.en.md) |
| `20-session-actor-dispatch` | [session/02-session-actor-binding](04-session/02-session-actor-binding.en.md) |
| `21-location-runtime` | [05-location-relocation/01-location-runtime](05-location-relocation/01-location-runtime.en.md) |
| `22-location-store-redis` | [05-location-relocation/02-location-store-redis](05-location-relocation/02-location-store-redis.en.md) |
| `23-relocation-store-redis` | [05-location-relocation/03-relocation-store-redis](05-location-relocation/03-relocation-store-redis.en.md) |
| `24-runtime-monitoring` | [06-observability/01-runtime-monitoring](06-observability/01-runtime-monitoring.en.md) |
| `25-runtime-metrics` | [06-observability/02-runtime-metrics](06-observability/02-runtime-metrics.en.md) |
| `26-message-flow-tracing` | [06-observability/03-message-flow-tracing](06-observability/03-message-flow-tracing.en.md) |
| `27-flow-correlation` | [06-observability/04-flow-correlation](06-observability/04-flow-correlation.en.md) |
| `28-relocation-flow` | [05-location-relocation/04-relocation-flow](05-location-relocation/04-relocation-flow.en.md) |
| `29-transport-liveness` | [02-channel-transport/05-transport-liveness](02-channel-transport/05-transport-liveness.en.md) |
| `30-host-relocation-flow` | [05-location-relocation/05-host-relocation-flow](05-location-relocation/05-host-relocation-flow.en.md) |
| `31-failure-failover-policy` | [05-location-relocation/06-failure-failover-policy](05-location-relocation/06-failure-failover-policy.en.md) |
| `32-framework-error-model` | [00-foundation/07-framework-error-model](00-foundation/07-framework-error-model.en.md) |
| `33-core-hwm-application-job-flow` | [01-execution/05-application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.en.md) |
| `40-internal-layering` | [00-foundation/08-layering](00-foundation/08-layering.en.md) |
| `41-internal-serialization` | [01-execution/02-handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.en.md) |
| `42-internal-progress-isolation` | §1–§4·§7 → [01-execution/02-handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.en.md) · §5·§6 → [05-application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.en.md) |
| `43-internal-completion` | [01-execution/01-submit-and-completion](01-execution/01-submit-and-completion.en.md) |
| `44-internal-relocation-continuity` | [05-location-relocation/04-relocation-flow](05-location-relocation/04-relocation-flow.en.md) |
| `45-internal-routing-and-cache` | §1·§1.1·§2 → [03-spot-actor/08-routing](03-spot-actor/08-routing.en.md) · §3–§7 → [02-channel-transport/02-channel-messaging](02-channel-transport/02-channel-messaging.en.md) |
| `46-internal-dispatch-loop` | §1·§2·§6·§8 → [01-execution/05-application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.en.md) · §7 → [04-spot-timer](03-spot-actor/10-spot-timer.en.md) · §3·§4 → [02-handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.en.md) |
| `47-internal-object-lifecycle` | [03-spot-actor/09-object-lifecycle](03-spot-actor/09-object-lifecycle.en.md) |
| `48-internal-session-binding` | [session/02-session-actor-binding](04-session/02-session-actor-binding.en.md) |
| `49-internal-liveness-and-state` | §1 → [02-channel-transport/05-transport-liveness](02-channel-transport/05-transport-liveness.en.md) · §2 → [03-spot-actor/03-mesh-node](03-spot-actor/03-mesh-node.en.md) · §3–§5 → [06-observability](06-observability/README.en.md) |
| `50-internal-message-ownership` | [01-execution/06-payload-ownership-and-codec](01-execution/05-payload-ownership-and-codec.en.md) |
| `51-internal-service-wire-protocol` | [02-channel-transport/06-wire-protocol](02-channel-transport/06-wire-protocol.en.md) |
| `52-internal-relocation-handoff` | [05-location-relocation/04-relocation-flow](05-location-relocation/04-relocation-flow.en.md) |
