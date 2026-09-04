---
title: "Foundation"
---

# Foundation

[Spec index](../README.en.md) · [Next: 01. Public Contract Governance](01-public-contract-governance.en.md)

> This topic covers the eight foundations shared across this entire spec: contract
> ownership; the meaning of common terms; what Framework does at the upper layer; how an
> operation's target and completion are determined; the form of a message; the public API
> family; common errors; and runtime layering boundaries.

## 1. What This Covers

Every other topic (execution, channel-transport, spot-actor, session, location-relocation,
observability) assumes what this topic defines. It defines who owns the public contract
and the procedure for changing it, exactly what the domain terms shared
across the whole spec mean, what Framework does in the upper layer that each language
implements independently, how a single message picks its target and when it is treated
as complete, what shape the typed payload, metadata, and codec of that message take, what
an application host must register at root, what common error an application receives when
send or request fails, and how runtime code is divided into chunks and which values must
not be merged into one.

This topic defines "what the contract covers." The channel-transport and spot-actor topics
respectively define "how that contract is physically delivered" (connection topology,
wire framing) and "how a [Spot](02-glossary.en.md#spot) — a logical instance with an address
and state — or Actor operates on top of that contract" (creation, relocation).

## 2. Documents in This Topic

| Document | Covers |
|---|---|
| [01. Public Contract Governance](01-public-contract-governance.en.md) | Definition of the public contract, the four ownership categories, what to pin down for a new contract, the 7-step public contract procedure, per-language representation principles, design review criteria |
| [02. Framework Messaging Glossary](02-glossary.en.md) | The single authority defining the domain terms (Spot, Actor, owner, generation, authority, …) shared across this entire spec |
| [03. Framework Overview](03-overview.en.md) | What Framework does, [MeshName](02-glossary.en.md#meshname)·[ChannelName](02-glossary.en.md#channelname)·[RouteMesh](02-glossary.en.md#routemesh) — the names identifying mesh and channel scopes, plus the physical connection scope itself —, message target selection, execution owner, connection management, what Framework hides |
| [04. Interaction Model](04-interaction-model.en.md) | The common model for Operation target selection and completion, send/request, Spot [Logical Multicast](02-glossary.en.md#logical-multicast), [classic fanout](02-glossary.en.md#classic-fanout), [STREAM session](02-glossary.en.md#stream-session), the effect of handler failure and termination |
| [05. Message Model](05-message-model.en.md) | Typed messages, `MessageContext`, `ActorRef`/`SpotRef` JSON, the `framework-json-v1` typed payload profile, application metadata, ownership and size limits |
| [06. Framework API](06-framework-api.en.md) | The language-neutral public API family — root registration, RouteMesh registration, messaging API, handler registration and filters, codec, Store registration, Spot·Actor·STREAM owner registration, startup validation |
| [07. Framework Error Model](07-framework-error-model.en.md) | The common `ErrorKind`, the completion/failure boundary of Send·Request, `CapacityExceeded` vs `Unavailable`, retry decisions |
| [08. Layering Boundaries and Identifiers](08-layering.en.md) | The binding boundary every language runtime follows, the shutdown procedure and cleanup order, when registration declarations are validated, the criteria for keeping identifiers separate (implementation spec) |

## 3. Find by Question

| Question | Where the answer is |
|---|---|
| Why are these spec documents split the way they are, and how are contract and implementation distinguished | [Public Contract Governance "1. What the Public Contract Is"](01-public-contract-governance.en.md#1-what-the-public-contract-is) · ["2. Contract Ownership"](01-public-contract-governance.en.md#2-contract-ownership) |
| What procedure is followed to add or change a public contract | [Public Contract Governance "5. Public Contract Procedure"](01-public-contract-governance.en.md#5-public-contract-procedure) |
| What exactly do the terms that recur throughout this spec (Spot, Actor, owner, generation, …) mean | [Framework Messaging Glossary](02-glossary.en.md) |
| What role does Framework play, and what does each language implement independently | [Framework Overview "1. What the Framework Does"](03-overview.en.md#1-what-the-framework-does) |
| What does each of MeshName·ChannelName·RouteMesh refer to | [Framework Overview "2. MeshName·ChannelName·RouteMesh"](03-overview.en.md#2-meshname-channelname-and-routemesh) |
| When a message is sent, how is the target decided, and when is it considered "complete" | [Interaction Model "1. Common Model — Target Selection and Completion"](04-interaction-model.en.md#1-common-model--target-selection-and-completion) |
| What is the difference between send and request, and under what conditions does each fail | [Interaction Model "4. Send and Request"](04-interaction-model.en.md#4-send-and-request) |
| How do Logical Multicast and classic fanout differ from each other | [Framework Overview "4. Logical Multicast and Classic Fanout"](03-overview.en.md#4-logical-multicast-and-classic-fanout) · [Interaction Model "5. Spot Logical Multicast"](04-interaction-model.en.md#5-spot-logical-multicast) · ["6. Classic Fanout"](04-interaction-model.en.md#6-classic-fanout) |
| What rules apply to a sent message's typed payload, metadata, and reply | [Message Model "1. Typed Messages"](05-message-model.en.md#1-typed-messages) |
| What are the JSON representation and codec rules for `ActorRef`·`SpotRef` | [Message Model "4. Global Object Reference JSON"](05-message-model.en.md#4-global-object-reference-json) · ["5. framework-json-v1 Typed Payload Profile"](05-message-model.en.md#5-the-framework-json-v1-typed-payload-profile) |
| What must an application host register at root for Framework to start | [Framework API "2. Root Registration"](06-framework-api.en.md#2-root-registration) |
| What key is a handler registered under, and when is a filter applied | [Framework API "9. Handler Registration and Dispatch"](06-framework-api.en.md#9-handler-registration-and-dispatch) · ["10. Handler Filter"](06-framework-api.en.md#10-handler-filter) |
| What common error does an application receive when Send·Request fails | [Framework Error Model](07-framework-error-model.en.md) |
| How are `CapacityExceeded` and `Unavailable` distinguished | [Framework Error Model "5. Request Completion and Failure"](07-framework-error-model.en.md#5-request-completion-and-failure) |
| How is runtime code divided, and which values must not be merged into one | [Layering Boundaries and Identifiers "6. Identifiers Are Not Merged"](08-layering.en.md#6-identifiers-are-not-merged) |
| How does startup validation differ from runtime validation | [Framework API "22. Startup Validation"](06-framework-api.en.md#23-startup-validation) · [Layering Boundaries and Identifiers "5. Registration Declarations Are Validated Only Once, at Startup"](08-layering.en.md#5-registration-declarations-are-validated-only-once-at-startup) |

## 4. Reading Order

**Developer reading for the first time**

1. Get the scope of this topic from §1 of this document.
2. Read [Framework Overview](03-overview.en.md) for what Framework does in the upper
   layer and the concepts of MeshName·ChannelName·RouteMesh and execution owner.
3. Read [Interaction Model "1. Common Model"](04-interaction-model.en.md#1-common-model--target-selection-and-completion)
   through ["4. Send and Request"](04-interaction-model.en.md#4-send-and-request) for
   message target selection and completion conditions.
4. Check the precise definition in the [glossary](02-glossary.en.md) whenever an unfamiliar
   term appears.

**Developer porting to a new language** — the documents below contain the public contract
and implementation decisions that every runtime must follow using the same structure, so
they must be read before language-specific implementation begins.

1. All of [Public Contract Governance](01-public-contract-governance.en.md) — the contract
   ownership boundary and public contract procedure must be understood first to know where
   to record the per-language interface of the new language.
2. All of [Framework API](06-framework-api.en.md) — the public API family and registration
   rules make up the largest part of this topic and define the standard already
   implemented by every other language runtime.
3. All of [Framework Error Model](07-framework-error-model.en.md) — the common
   `ErrorKind` and completion boundary are a contract that the language-specific
   exception and result representation must follow as-is.
4. All of [Layering Boundaries and Identifiers](08-layering.en.md) — the binding
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
| RouteMesh·ClientServer physical connections, wire framing, transport liveness | [channel-transport topic](../02-channel-transport/README.en.md) |
| Spot·Actor creation, membership, and relocation procedure | [spot-actor topic](../03-spot-actor/README.en.md) |
| The detailed procedures for STREAM session and Actor binding | [Session topic](../04-session/README.en.md) |
| The source and target execution flow of Actor·Spot relocation | [location-relocation topic](../05-location-relocation/README.en.md) |
| Runtime monitoring, message flow tracing, flow correlation | [observability topic](../06-observability/README.en.md) |
| Completion races for Send·Request, execution turn, detailed backpressure rules | [execution topic](../01-execution/README.en.md) |

---

[Spec index](../README.en.md) · [Next: 01. Public Contract Governance](01-public-contract-governance.en.md)
