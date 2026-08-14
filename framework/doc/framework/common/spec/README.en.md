# Framework Common Spec

The documents in this directory describe the Framework's common public
contract. Each document self-contains the inputs, state, normal flow, and
failure/completion conditions its implementation and contract tests need.

This directory and the per-language exact interfaces are the single authority
for the Framework public contract. Documents `40` through `52` in this directory
are internal design documentation: they explain the state and component structure
used to implement that contract and do not add public behavior. Co-location does
not make those documents normative; when public behavior differs, documents `00`
through `32` and the per-language exact interfaces prevail.

## Verification Runner Isolation

Samples and E2E suites for multiple language implementations of the same contract
must be runnable concurrently on one host. This is a contract-verification
environment rule, not public API behavior. A run that needs Redis does not share
one instance per language or separate only by Redis database number. It creates a
dedicated Docker Redis container and key prefix for every run.

Samples use the language-specific `20000-29999` ranges defined by the
[sample runner isolation standard](../sample/README.en.md#the-sample-run-script-and-redis-isolation-standard).
E2E uses the language-specific `30000-39999` ranges defined by the
[E2E runner execution contract](../e2e/README.en.md#27-run_e2e-execution-contract).
The tables in those documents own the exact non-overlapping Redis host-port and
application-listener ranges.

A standalone config runner and every config runner invoked by an aggregate run
share a language-wide whole-run lock, so actual config E2E processes execute
sequentially within one language. The aggregate runner itself stays lock-free,
so two aggregate runs may alternate at config boundaries, but their actual
config processes never overlap. The same E2E can run concurrently in different
languages because language-specific port ranges, per-run Redis endpoints,
temporary configuration, log directories, and cleanup targets are separate. Java
and Kotlin share some Gradle output, so a build-only lock shared by sample and E2E
runners serializes only Gradle execution. This lock is shared within one runner
execution environment. Running WSL Bash and Windows PowerShell against the same
checkout at the same time is unsupported because they use different
operating-system lock namespaces.

## Authoring Standards And Shared Terms

- [Spec writing guide](../../../../../doc/principal/documentation/spec-writing-guide.ko.md)

## Topic-Based Navigation

The `40`–`52` entries are non-normative internal-design documents that add no public contract. This
index places public specs and internal designs together by topic and lists each document once.

### Foundation And Configuration

- [00 Public contract governance](00-public-contract-governance.en.md)
- [01 Framework messaging glossary](01-glossary.en.md)
- [02 Framework overview](02-overview.en.md)
- [03 Interaction model](03-interaction-model.en.md)
- [05 Async execution policy](05-async-execution-policy.en.md)
- [06 Framework API](06-framework-api.en.md)
- [10 Network listener identity](10-network-listener-identity.en.md)

### Messaging, HWM, And Backpressure

- [04 Message model](04-message-model.en.md)
- [07 RouteMesh topology](07-channel-topology.en.md)
- [08 Channel messaging](08-channel-messaging.en.md)
- [09 ClientServer Channel](09-client-server-channel.en.md)
- [12 Spot messaging](12-spot-messaging.en.md)
- [17 Stage wrapper on Spot](17-stage-wrapper-on-spot.en.md)
- [32 Framework error model](32-framework-error-model.en.md) — defines the shared `ErrorKind`, Send/Request completion conditions, and the boundary of an application's retry decision.
- [46. Receive And Dispatch Loop](46-internal-dispatch-loop.en.md) — non-normative internal design. Whether to wake per message or batch-process. What wakes it

### Spot, Actor, And Session

- [11 Spot model](11-spot-model.en.md)
- [13 MeshNode](13-mesh-node.en.md)
- [14 Actor model](14-actor-model.en.md)
- [15 Spot and Actor membership](15-spot-actor.en.md)
- [16 Spot address messaging](16-spot-address-messaging.en.md)
- [18 Spot/Actor routing](18-object-routing.en.md)
- [19 STREAM server session](19-stream-session.en.md)
- [20 Session Actor dispatch](20-session-actor-dispatch.en.md)
- [41. Spot · Actor Execution Serialization](41-internal-serialization.en.md) — non-normative internal design. Why the queueing spot and execution authority are separated. Why execution resource mustn't be proportional to Spot count
- [47. Object Kind And Activation](47-internal-object-lifecycle.en.md) — non-normative internal design. How the three Spot kinds are distinguished. When a missing object is built and how Ready owner failure is handled
- [48. Session And Actor Binding](48-internal-session-binding.en.md) — non-normative internal design. How to keep two places from pointing at the same Actor while a connection is swapped

### Location, Relocation, And Handoff

- [21 Location runtime](21-location-runtime.en.md) — defines the order in which the Framework uses object location, authority, and the two Stores.
- [22 Location Store provider SPI and the official Redis implementation](22-location-store-redis.en.md) — defines the atomic key/value and scan contract a provider must implement.
- [23 Relocation Store provider SPI and the official Redis implementation](23-relocation-store-redis.en.md) — defines the immutable payload storage contract a provider must implement.
- [28 Complete Actor and Spot relocation flow](28-relocation-flow.en.md) — defines the owner transition, queue merge, Location Store CAS, and Session route order shared by all four runtimes.
- [30 Complete Host Relocation Flow](30-host-relocation-flow.en.md) — defines the complete lifecycle in which a Host fixes relocation units, moves them in batch order, returns `Relocated`, retains Message Follow, and finishes with `Shutdown`.
- [31 Failure handling and failover scope](31-failure-failover-policy.en.md) — defines the automatic-recovery boundary for target reselection, reconnect, creation recovery, and stateful relocation.
- [44. Message Continuity During A Move](44-internal-relocation-continuity.en.md) — non-normative internal design. Where a message goes while an object is moving
- [45. Target Selection And Route Cache](45-internal-routing-and-cache.en.md) — non-normative internal design. How often location is looked up. How `Missing` differs from a `Ready` owner that can't be used
- [52. Relocation Handoff State Transitions](52-internal-relocation-handoff.en.md) — non-normative internal design. How all four runtimes implement the same source, target, and Session transitions and queue order

### Monitoring And Operations

- [24 Runtime state and operational diagnostics](24-runtime-monitoring.en.md) — defines the health, topology status, and structured logs an application reads.
- [25 Runtime metric names and labels](25-runtime-metrics.en.md) — defines only metric names, units, and bounded labels.
- [26 Message flow tracing](26-message-flow-tracing.en.md) — defines the phases, outcomes, and trace attributes of a single message.
- [27 Request correlation and causal flow](27-flow-correlation.en.md) — defines the generation and propagation of the correlation ID and flow ID.
- [29 Transport liveness](29-transport-liveness.en.md)
- [49. Liveness And Status Publication](49-internal-liveness-and-state.en.md) — non-normative internal design. How to determine whether the peer is still reachable without letting that judgment change authority

### Runtime Ownership And Wire Protocol

- [40. Layer Boundary And Identifier](40-internal-layering.en.md) — non-normative internal design. Where to draw the binding boundary. Which values mustn't be merged
- [42. Application And Infrastructure Execution Separation](42-internal-progress-isolation.en.md) — non-normative internal design. What must still progress even while a handler is stuck. Why it's a region separation, not a reserved section
- [43. Operation Completion Confirmation](43-internal-completion.en.md) — non-normative internal design. How to make only one win when multiple paths try to finish at once. How not to lose a response
- [50. Payload Ownership And Copy](50-internal-message-ownership.en.md) — non-normative internal design. How many times a byte is copied from socket to handler. When deserialization happens
- [51. Service Wire Protocol](51-internal-service-wire-protocol.en.md) — non-normative internal design. The byte format and command exchanged between nodes

## Internal Design Documents (Non-Normative)

> **Document status — internal design, not normative public specification.** The `40`–`52` documents below explain implementation structure used to satisfy the public contracts in `00`–`32`. They do not add or change application-visible behavior.

The C++, .NET, JVM, and Node.js service runtimes are implemented in different
languages. This document set explains the **internal design decisions they must share
to give an application the same result.**

### What This Document Set Answers

The formal spec decides "what must be built." This document set
answers what can't be known just by reading the spec.

- **What structure is needed to satisfy several spec requirements together.** For
  example, it explains the structure that preserves per-Actor queues while also
  serializing all `SpotWide` execution.
- **Which criteria select an internal implementation where the spec does not.**
- **Which boundaries tend to diverge across implementations, and what must be
  verified there.**

Content the spec already decided isn't repeated — only a link is put.

A `Decision` in this document set is not a public contract; it is an internal
structure decision for satisfying that contract. A `Result To Confirm` checks
the spec's public result and internal invariants in an implementation and does
not create a new user guarantee. If public behavior, error meaning, or failover
scope differs from the spec, the spec prevails. Internals are then aligned to
the spec, or, if the public contract itself must change, the
[public-contract procedure](00-public-contract-governance.en.md#4-public-contract-procedure)
is followed first.

Deviations from these decisions and verification progress are not recorded in this public
internals document. This document describes only implementation structure and decisions.

### Component And Responsible Chapter

Each chapter explains one component marked in the diagram below. Start at the component
you need and follow its chapter number.

**This diagram is a chapter-finding map, not a layer diagram.** The
left bundle and the right bundle are **different processes**, and even
if one host plays both roles, the diagram's two spots each operate in
a different call.

```mermaid
flowchart LR
    subgraph SEND["sender process"]
        SEL["selector · route cache<br/>「45」"]
    end

    subgraph WIRE["between processes"]
        direction TB
        TR["peer connection · liveness<br/>「49」"]
        REC["service wire record<br/>「wire」"]
    end

    subgraph OWNER["owner process"]
        direction TB
        RL["receive loop<br/>「46」"]
        AD["admission<br/>「46」"]
        GATE["execution gate<br/>「41」「42」"]
        H["application handler"]
        FIN["completion<br/>「43」"]
    end

    subgraph STATE["owner process state"]
        direction TB
        OBJ["Spot · Actor<br/>「47」"]
        SB["session binding<br/>「48」"]
        MV["relocation · Message Follow<br/>「44」"]
    end

    COD["codec · payload ownership<br/>「50」"]
    LS[("Location Store")]
    OBS["status · metric<br/>「49」"]

    SEL --> TR --> REC --> RL --> AD --> GATE --> H --> FIN
    FIN -. "response" .-> TR
    SEL -. "lookup" .-> LS
    AD -. "confirms owner" .-> OBJ
    OBJ --- SB
    OBJ --- MV
    MV -. "invalidates cache" .-> SEL
    SEL -. "serializes" .-> COD
    COD -. "deserializes" .-> H
    GATE -. "doesn't occupy" .-> OBS
```

**A solid line is an axis a message actually crosses, and a dotted
line is a reference/lookup/notification.** "1. Layer Boundary And
Identifier" spans this whole diagram — since it decides which
component may know a binding type, it isn't placed in one spot.

The reason `codec` and `Location Store` are put outside the bundle is
that both processes use them. codec serializes on the sending side and
deserializes after moving ownership on the receiving side, and Location
Store is each looked up and recorded by both processes. Putting them
inside one bundle would read as if only that process uses them.

The two dotted lines are specifically marked because they're a
connection easy to miss when reading a chapter separately.

- `execution gate → status · metric`'s **"doesn't occupy"** — the
  decision from [49](49-internal-liveness-and-state.en.md) that observation
  must bypass execution authority. Turning on observation must not
  slow down processing.
- `relocation → selector`'s **"invalidates cache"** — the point where
  [44](44-internal-relocation-continuity.en.md) and
  [45](45-internal-routing-and-cache.en.md) meet. Without this line, every
  traffic detours until the cache lifetime ends after a move.

A performance-critical decision is gathered in
[50](50-internal-message-ownership.en.md)'s copy count,
[45](45-internal-routing-and-cache.en.md)'s location cache,
[46](46-internal-dispatch-loop.en.md)'s batching/wake method/timer resource,
[41](41-internal-serialization.en.md)'s execution resource constraint, and
[47](47-internal-object-lifecycle.en.md)'s memory accounting.

### Structure Decisions That Span Chapters

Some topics are covered by multiple documents. The spec is authoritative for
public behavior; align internal structure to the following documents.

| Topic | Reference Document |
|---|---|
| The public result when a queue saturates | The family × location table in [Spot Messaging 「5.3 Work Put On The Spot Application Queue」](12-spot-messaging.en.md#53-work-put-on-the-spot-application-queue) |
| The owner-occupancy bound and the lifecycle continuous-execution bound | [Actor Model 「3. Actor Queue」](14-actor-model.en.md#3-actor-queue) |
| The target-selection procedure and tiebreak | [Channel Messaging 「Selection Order」](08-channel-messaging.en.md#selection-order) |
| Observer merging and loss | [Runtime Status And Operational Diagnostics](24-runtime-monitoring.en.md) |
| Where `ObjectGeneration` is used and where it isn't | [Spot · Actor Routing 「2.5」](18-object-routing.en.md#25-where-objectgeneration-is-used-and-where-its-not) |

### Debugging Principles

When chasing an intermittent failure, **turn on the message tracking and file logs
that already exist and read them first.** Adding fresh temporary logging and
re-running the reproduction is not allowed. That approach spends a whole
reproduction cycle to see a single exception, and it misses causes that were
already printed in the existing logs.

#### 1. What To Turn On First

| Target | How |
|---|---|
| Message flow (full-path tracing with `flow` and `corr`) | runtime diagnostics message flow mode |
| C++ / .NET spot discovery trace | `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY` |
| Java / Kotlin stream trace | `ZLINK_JAVA_STREAM_TRACE=1` |
| Sample server log retention | .NET `ZLINK_SAMPLE_EVIDENCE_DIR`, JVM `ZLINK_SAMPLE_KEEP_RUN_DIR=1`, Node keeps them on failure automatically |

When a sample fails intermittently, retain server logs **from the first
reproduction**. A run without logs records only that it failed, not why, so that
cycle is wasted.

#### 2. How To Read Them

Put a passing case and a failing case side by side under `flow` and find **which
transition stopped**. `flow` is the only value that ties one message across
process boundaries. Filtering a whole trace category out as noise walks straight
past the line that names the cause.

#### 3. Every Failure Belongs On The Flow

Never build a terminal that hands the application an error kind and drops the
cause. A failure with no recorded cause can only be traced by reproducing it, and
the reproduction cycle becomes the cost of the investigation. Failures,
refusals, and aborts are recorded as `message_flow_outcome` `error`, carrying the
originating exception in `errorType` / `errorMessage`, **under the same `flow` as
the message that produced them**.

#### 4. Cost Rule For Adding Traces

**Decision**: when message flow tracing is off, building the log message must cost
nothing.

| Path | Method |
|---|---|
| Hot path traced per message | Wrap in `if (enabled(outcome))` so neither the event nor a lambda is built |
| Rare transitions such as failure or abort | Use the lazy form (`trace(outcome, build)` / `traceLazy`) so the event is built only after the gate |

The lazy form removes the `if` at the call site but allocates one lambda (C++
inlines it, so nothing is allocated). Hot paths therefore wrap even the lazy form
in an `if`, so no lambda is created either. Never write a call site that
concatenates strings before the gate.

**Language discretion**: how the gate is expressed. C++ uses a template lambda,
.NET an interpolated string handler and `Func<>`, Java a `Supplier<>`, Node a
thunk. What must match is the observable result — no cost at all when off.

**Result to verify**: after adding a trace, confirm from the call-site code that
with tracing off the path builds no string, no event, and no lambda.

### How To Read

Each document states the following for every decision.

| Mark | Meaning |
|---|---|
| **Decision** | The structure every service runtime must follow. Violating it changes the result the application sees |
| **Per-Language Discretion** | What's fine to implement differently as long as the observed result is the same. Forcing them to match becomes unnatural in that language |
| **Result To Confirm** | The condition the implementation must satisfy. The confirmation method differs per item |

**Writing something as discretion requires two things together**: why the observable
result is the same, and the standard that confirms it. Missing either one means it is
not discretion but something not yet decided. A choice that produces an observable
difference such as a latency floor is written as a **constrained choice**, not
discretion (see [46. Receive And Dispatch Loop 「5. Pick One Wake-Up
Method」](46-internal-dispatch-loop.en.md#5-pick-one-wake-up-method)).

**A runtime must not invent a refusal condition, retry, or record that
isn't documented.** If such behavior changes the application's observable
result, add a common decision first and require every runtime to follow it.

Only the wire protocol document doesn't apply this distinction. It's
paired with
`framework/runtime/protocol/service-wire-v1.schema.json`, and explains
the field relationship and validation order the schema decides.

Not every "result to confirm" can be judged by a contract test. The
confirmation method differs per item, and when moving the list into
work, first decide which method to confirm it with.

#### Citation Notation

A citation is by **section title**. Clicking the link jumps directly to
that section.

```markdown
[Actor Model 「3. Actor Queue」](14-actor-model.en.md#3-actor-queue)
```

**Don't cite by line number.** A `§123` form only jumps to the top of
the document, forcing the reader to find that spot again, and even one
line changing in the cited document throws off where it points. A
section title only breaks when that section disappears or is renamed,
and that's revealed by link checking at that time.

The anchor is the title lowercased with a space joined by `-`.
Confirmed with the following.

```bash
mkdocs build --strict   # run from doc/site
```

| Confirmation Method | Which Item |
|---|---|
| Contract test | A result the application observes — error kind, order, whether a callback is called |
| White-box invariant | Runtime-internal state — queue occupancy, execution authority count, state transition |
| Static check | Code structure — type leak, duplicate implementation, a prohibited include |
| Measurement | Cost — allocation count, lock-acquisition count, throughput. A threshold must be decided first to judge |

For example, "don't run two work items concurrently in one execution
authority" is a decision, and whether to build it with promise
chaining or with a lock and queue is discretion.

### What This Document Set Doesn't Define

| Content | Owning Document |
|---|---|
| The name and signature of an API the application calls | [Per-Language Public Contract](server/languages/README.en.md) |
| The meaning and completion condition of public behavior | [Formal Spec](README.en.md) |
| The raw socket/transport internal Core provides | [Core Raw Runtime Internal Boundary](https://zlink-systems.github.io/zlink/internals/runtime-boundary/) |

Each runtime implements this document's meaning in independent source;
sharing a common native binary isn't required.

## Server Exact Interface Per Language

The exact public types, signatures, and async representation each language
uses for the common server contract are owned by the following documents.

- [C++](server/languages/cpp/README.en.md)
- [.NET](server/languages/dotnet/README.en.md)
- [Java](server/languages/java/README.en.md)
- [Kotlin](server/languages/kotlin/README.en.md)
- [Node.js](server/languages/node/README.en.md)

## HTTP Client

- [HTTP client spec index](http-client/README.en.md)
- [12 HTTP client integration contract](http-client/12-http-client.en.md)
- [Per-language HTTP client contract](http-client/language-interfaces.en.md)

`10-revision-candidates.ko.md` is not a public contract — it's a document
that manages design candidates for the next revision.

## Stream Connector

- [32 Stream connector](stream-connector/32-stream-connector.en.md)
- [Per-language Stream connector contract](stream-connector/README.en.md#per-language-public-api)
