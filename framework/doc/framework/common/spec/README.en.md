# Framework Common Spec

The documents in this directory describe the Framework's common public
contract. Each document self-contains the inputs, state, normal flow, and
failure/completion conditions its implementation and contract tests need.

## Authoring Standards And Shared Terms

- [Spec writing guide](../../../../../doc/principal/documentation/spec-writing-guide.ko.md)
- [00 Public contract governance](00-public-contract-governance.en.md)
- [01 Framework messaging glossary](01-glossary.en.md)

## Base Contract

- [02 Framework overview](02-overview.en.md)
- [03 Interaction model](03-interaction-model.en.md)
- [04 Message model](04-message-model.en.md)
- [05 Async execution policy](05-async-execution-policy.en.md)
- [06 Framework API](06-framework-api.en.md)

## Channel And Network

- [07 RouteMesh topology](07-channel-topology.en.md)
- [08 Channel messaging](08-channel-messaging.en.md)
- [09 ClientServer Channel](09-client-server-channel.en.md)
- [10 Network listener identity](10-network-listener-identity.en.md)

## Object Messaging

- [11 Spot model](11-spot-model.en.md)
- [12 Spot messaging](12-spot-messaging.en.md)
- [13 MeshNode](13-mesh-node.en.md)
- [14 Actor model](14-actor-model.en.md)
- [15 Spot and Actor membership](15-spot-actor.en.md)
- [16 Spot address messaging](16-spot-address-messaging.en.md)
- [17 Stage wrapper on Spot](17-stage-wrapper-on-spot.en.md)
- [18 Spot/Actor routing](18-object-routing.en.md)

## STREAM And Sessions

- [19 STREAM server session](19-stream-session.en.md)
- [20 Session Actor dispatch](20-session-actor-dispatch.en.md)

## Location Store And Relocation

- [21 Location runtime](21-location-runtime.en.md) — defines the order in which the Framework uses object location, authority, and the two Stores.
- [22 Location Store provider SPI and the official Redis implementation](22-location-store-redis.en.md) — defines the atomic key/value and scan contract a provider must implement.
- [23 Relocation Store provider SPI and the official Redis implementation](23-relocation-store-redis.en.md) — defines the immutable payload storage contract a provider must implement.

## Observability And Termination

- [24 Runtime state and operational diagnostics](24-runtime-monitoring.en.md) — defines the health, topology status, and structured logs an application reads.
- [25 Runtime metric names and labels](25-runtime-metrics.en.md) — defines only metric names, units, and bounded labels.
- [26 Message flow tracing](26-message-flow-tracing.en.md) — defines the phases, outcomes, and trace attributes of a single message.
- [27 Request correlation and causal flow](27-flow-correlation.en.md) — defines the generation and propagation of the correlation ID and flow ID.
- [28 Host Relocate and Shutdown](28-graceful-drain-handoff.en.md) — defines the two relocation modes and the shutdown lifecycle.
- [29 Transport liveness](29-transport-liveness.en.md)
- [31 Failure handling and failover scope](31-failure-failover-policy.en.md) — defines the automatic-recovery boundary for target reselection, reconnect, creation recovery, and stateful relocation.
- [32 Framework error model](32-framework-error-model.en.md) — defines the shared `ErrorKind`, Send/Request completion conditions, and the boundary of an application's retry decision.

## Server Exact Interface Per Language

The exact public types, signatures, and async representation each language
uses for the common server contract are owned by the following documents.

- [C++](server/languages/cpp/README.ko.md)
- [.NET](server/languages/dotnet/README.ko.md)
- [Java](server/languages/java/README.ko.md)
- [Kotlin](server/languages/kotlin/README.ko.md)
- [Node.js](server/languages/node/README.ko.md)

## HTTP Client

- [HTTP client spec index](http-client/README.en.md)
- [12 HTTP client integration contract](http-client/12-http-client.en.md)
- [Per-language HTTP client contract](http-client/language-interfaces.en.md)

`10-revision-candidates.ko.md` is not a public contract — it's a document
that manages design candidates for the next revision.

## Stream Connector

- [32 Stream connector](stream-connector/32-stream-connector.en.md)
- [Per-language Stream connector contract](stream-connector/README.en.md#per-language-public-api)
