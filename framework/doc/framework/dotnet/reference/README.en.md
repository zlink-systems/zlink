# .NET Framework Reference

Authoring rules follow the
[reference-writing guide](../../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only, like the rest of `doc/principal/`). This document is that guide applied to this
tree.

> The two documents linked below are part of the framework doc tree, which stays Korean-only.
> Following those links lands on Korean content.

This document plays a different role from two existing documents.

- [Guide 13. Interface catalog](../guide/server/13-interface-catalog.ko.md) — introduces
  frequently used interfaces from a tutorial angle.
- [.NET exact interface](../../common/spec/server/languages/dotnet/interfaces/README.ko.md) —
  the contract-owning document. It carries the full interface, signature for signature.
- **This reference** — collects only "what you need to know to complete this one call."
  It does not duplicate the contract text; it cites the exact interface instead.

## Entry unit

One reference entry = **one entry-point method.** A method that produces an awaitable result
ending in a terminal `Async()`/`Async<TReply>()`/`Yield<TReply>()` — `RequestToChannel`,
`SendToChannel`, `Publish` — is an entry.

Modifiers such as `.Timeout(...)` or `.Metadata(...)` that the builder returned by that method
exposes never become their own entry. They only appear inside that entry's "Options" table.
A document that lists fluent-builder components method by method fails to answer what the
caller actually needs — "what does this one call do" — and this rule exists to avoid that
failure.

## Category

The chapters use the same 8 categories as the public-contract audit categorization
([contract-inventory](../../../contract-inventory/route-mesh-v11-public-contract-trace.json)).
This taxonomy is already verified across languages, so this tree does not invent a new one.

| Category | Status | Corresponding spec |
|---|---|---|
| [Host lifecycle](01-host-lifecycle.en.md) | Complete | 06-framework-api, 28-graceful-drain-handoff |
| [Topology discovery](02-topology-discovery.en.md) | Complete | 07-channel-topology, 09-client-server-channel, 10-network-listener-identity, 21-location-runtime |
| [Messaging execution](03-messaging-execution.en.md) | Complete | 04-message-model, 05-async-execution-policy, 08-channel-messaging |
| [Spot instance](04-spot-instance.en.md) | Complete | 12-spot-messaging, 15-spot-actor, 16-spot-address-messaging, 17-stage-wrapper-on-spot |
| [Actor relocation](05-actor-relocation.en.md) | Complete | 14-actor-model, 15-spot-actor, 20-session-actor-dispatch, 28-graceful-drain-handoff |
| [Stream session](06-stream-session.en.md) | Complete | 19-stream-session, 20-session-actor-dispatch |
| [Location authority](07-location-authority.en.md) | Complete | 21-location-runtime, 22-location-store-redis, 23-relocation-store-redis, 28-graceful-drain-handoff |
| [Observability diagnostics](08-observability-diagnostics.en.md) | Complete | 24-runtime-monitoring, 25-runtime-metrics, 26-message-flow-tracing, 27-flow-correlation, 29-transport-liveness |

Numbering follows this same order (the same order contract-inventory uses for its categories).

ko and en are both complete, and the same structure has been extended to the remaining four
framework languages (C++, Java, Kotlin, Node.js). This document tree is wired into `mkdocs.yml`
nav.
