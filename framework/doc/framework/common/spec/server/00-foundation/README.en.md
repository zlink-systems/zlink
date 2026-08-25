---
title: "Foundation"
---

# Foundation

[Spec index](../README.en.md) · [Next: 01. Public Contract Governance](01-public-contract-governance.en.md)

> The foundation this entire spec shares — who owns the contract, what the common terms
> mean, what Framework does above the runtime, how an operation picks its target and
> completion, what the shape of a message and the public API family are, and what the
> common errors and runtime layering boundaries are — this topic covers those eight
> foundations.

## 1. What This Covers

Every other topic (execution, channel-transport, spot-actor, session, location-relocation,
observability) is written on top of what this topic defines. It defines who owns the
public contract and by what procedure it changes, exactly what the domain terms shared
across the whole spec mean, what Framework does in the upper layer that each language
implements independently, how a single message picks its target and when it is treated
as complete, what shape the typed payload, metadata, and codec of that message take, what
an application host must register at root, what common error an application receives when
send or request fails, and how runtime code is divided into chunks and which values must
not be merged into one.

This topic defines "what is contracted"; "how that contract is physically delivered"
(connection topology, wire framing) and "how a [Spot](02-glossary.en.md#spot) — a logical
instance with an address and state — or Actor lives on top of that contract" (creation,
relocation) are defined by the channel-transport and spot-actor topics respectively.

## 2. Documents In This Topic

| Document | Covers |
|---|---|
| [01. Public Contract Governance](01-public-contract-governance.en.md) | Definition of the public contract, the four ownership categories, what to pin down for a new contract, the 7-step public contract procedure, per-language representation principles, design review criteria |
| [02. Framework Messaging Glossary](02-glossary.en.md) | The single authority defining the domain terms (Spot, Actor, owner, generation, authority, …) shared across this entire spec |
| [03. Framework Overview](03-overview.en.md) | What Framework does, [MeshName](02-glossary.en.md#meshname)·[ChannelName](02-glossary.en.md#channelname)·[RouteMesh](02-glossary.en.md#routemesh) — the names identifying mesh and channel scope and that physical connection scope itself —, message target selection, execution owner, connection management, what Framework hides |
| [04. Interaction Model](04-interaction-model.en.md) | The common model for Operation target selection and completion, send/request, Spot [Logical Multicast](02-glossary.en.md#logical-multicast), [classic fanout](02-glossary.en.md#classic-fanout), [STREAM session](02-glossary.en.md#stream-session), the effect of handler failure and termination |
| [05. Message Model](05-message-model.en.md) | Typed messages, `MessageContext`, `ActorRef`/`SpotRef` JSON, the `framework-json-v1` typed payload profile, application metadata, ownership and size limits |
| [06. Framework API](06-framework-api.en.md) | The language-neutral public API family — root registration, RouteMesh registration, messaging API, handler registration·filter, codec, Store registration, Spot·Actor·STREAM owner registration, startup validation |
| [07. Framework Error Model](07-framework-error-model.en.md) | The common `ErrorKind`, the completion/failure boundary of Send·Request, `CapacityExceeded` vs `Unavailable`, retry judgment |
| [08. Layering Boundaries And Identifiers](08-layering.en.md) | The binding boundary every language runtime follows, the shutdown procedure and cleanup order, when registration declarations are validated, the criteria for not merging identifiers (implementation spec) |

## 3. Find By Question

| Question | Where the answer is |
|---|---|
| Why are these spec documents split the way they are, and how are contract and implementation distinguished | [Public Contract Governance "1. What Is A Public Contract"](01-public-contract-governance.en.md#1-what-the-public-contract-is) · ["2. Contract Ownership"](01-public-contract-governance.en.md#2-contract-ownership) |
| What procedure is followed to add or change a new public contract | [Public Contract Governance "5. Public Contract Procedure"](01-public-contract-governance.en.md#5-public-contract-procedure) |
| What exactly do the terms that recur throughout this spec (Spot, Actor, owner, generation, …) mean | [Framework Messaging Glossary](02-glossary.en.md) |
| What layer is Framework, and what does each language implement independently | [Framework Overview "1. What Framework Does"](03-overview.en.md#1-what-the-framework-does) |
| What do MeshName·ChannelName·RouteMesh each refer to | [Framework Overview "2. MeshName·ChannelName·RouteMesh"](03-overview.en.md#2-meshname-channelname-and-routemesh) |
| When a message is sent, how is the target decided, and when is it considered "complete" | [Interaction Model "1. Common Model — Target Selection And Completion"](04-interaction-model.en.md#1-common-model--target-selection-and-completion) |
| What is the difference between send and request, and under what conditions does each fail | [Interaction Model "4. Send And Request"](04-interaction-model.en.md#4-send-and-request) |
| How do Logical Multicast and classic fanout differ from each other | [Framework Overview "4. Logical Multicast And Classic Fanout"](03-overview.en.md#4-logical-multicast-and-classic-fanout) · [Interaction Model "5. Spot Logical Multicast"](04-interaction-model.en.md#5-spot-logical-multicast) · ["6. Classic Fanout"](04-interaction-model.en.md#6-classic-fanout) |
| What rules does the shape of a sent message (typed payload, metadata, reply) follow | [Message Model "1. Typed Messages"](05-message-model.en.md#1-typed-messages) |
| What are the JSON representation and codec rules for `ActorRef`·`SpotRef` | [Message Model "4. Global Object Reference JSON"](05-message-model.en.md#4-global-object-reference-json) · ["5. framework-json-v1 Typed Payload Profile"](05-message-model.en.md#5-the-framework-json-v1-typed-payload-profile) |
| What must an application host register at root for Framework to start | [Framework API "2. Root Registration"](06-framework-api.en.md#2-root-registration) |
| What key is a handler registered under, and when is filter applied | [Framework API "9. Handler Registration And Dispatch"](06-framework-api.en.md#9-handler-registration-and-dispatch) · ["10. Handler Filter"](06-framework-api.en.md#10-handler-filter) |
| What common error does an Application receive when Send·Request fails | [Framework Error Model](07-framework-error-model.en.md) |
| How are `CapacityExceeded` and `Unavailable` distinguished | [Framework Error Model "5. Request Completion And Failure"](07-framework-error-model.en.md#5-request-completion-and-failure) |
| What chunks is runtime code divided into, and which values must not be merged into one | [Layering Boundaries And Identifiers "6. Identifiers Are Not Merged"](08-layering.en.md#6-identifiers-are-not-merged) |
| How does what is validated at startup differ from what is validated at runtime | [Framework API "22. Startup Validation"](06-framework-api.en.md#22-startup-validation) · [Layering Boundaries And Identifiers "5. Registration Declarations Are Validated Only Once, At Startup"](08-layering.en.md#5-registration-declarations-are-validated-only-once-at-startup) |

## 4. Reading Order

**Developer reading for the first time**

1. Get the scope of this topic from §1 of this document.
2. Read [Framework Overview](03-overview.en.md) for what Framework does in the upper
   layer and the concepts of MeshName·ChannelName·RouteMesh and execution owner.
3. Read [Interaction Model "1. Common Model"](04-interaction-model.en.md#1-common-model--target-selection-and-completion)
   through ["4. Send And Request"](04-interaction-model.en.md#4-send-and-request) for
   message target selection and completion conditions.
4. Check the precise definition in the [glossary](02-glossary.en.md) whenever an unfamiliar
   term appears.

**Developer porting to a new language** — the documents below hold the public contract
and implementation decisions that every runtime must follow with the same structure, so
they are read before language-specific implementation begins.

1. All of [Public Contract Governance](01-public-contract-governance.en.md) — the contract
   ownership boundary and public contract procedure must be understood first to know where
   to record the per-language interface of the new language.
2. All of [Framework API](06-framework-api.en.md) — the public API family and registration
   rules are the largest by volume in this topic, and are the standard every other
   language runtime has already implemented.
3. All of [Framework Error Model](07-framework-error-model.en.md) — the common
   `ErrorKind` and completion boundary are a contract that the language-specific
   exception·result representation must follow as-is.
4. All of [Layering Boundaries And Identifiers](08-layering.en.md) — the binding
   boundary, shutdown procedure, and identifier separation are the implementation spec,
   and include the verification requirement (§7).

**Application developer**

1. Get the whole picture from [Framework Overview](03-overview.en.md).
2. Read [Framework API "2. Root Registration"](06-framework-api.en.md#2-root-registration)
   through ["6. Messaging API Family"](06-framework-api.en.md#6-messaging-api-family) for
   root registration and the public API for sending messages.
3. Check the common error received on failure in
   [Framework Error Model](07-framework-error-model.en.md).

## 5. What This Topic Does Not Define

| Content | Owning document |
|---|---|
| RouteMesh·ClientServer physical connection, wire framing, transport liveness | [channel-transport topic](../02-channel-transport/README.en.md) |
| Spot·Actor creation, membership, and relocation procedure | [spot-actor topic](../03-spot-actor/README.en.md) |
| The detailed procedure of STREAM session and Actor binding | [Session topic](../04-session/README.en.md) |
| The source·target execution flow of Actor·Spot relocation | [location-relocation topic](../05-location-relocation/README.en.md) |
| Runtime monitoring, message flow tracing, flow correlation | [observability topic](../06-observability/README.en.md) |
| The completion race of Send·Request, execution turn, detailed backpressure rules | [execution topic](../01-execution/README.en.md) |

---

[Spec index](../README.en.md) · [Next: 01. Public Contract Governance](01-public-contract-governance.en.md)
