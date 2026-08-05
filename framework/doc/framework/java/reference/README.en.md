# Java Framework Reference

The writing rules follow the
[Reference-writing guide](../../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This tree reuses the same 8 categories and order as the dotnet reference (the
parity-reference lane), and each entry was written by cross-checking the Java exact interface
(the `javap` inventory) directly.

> The guide and exact-interface documents linked below stay Korean-only. Following those links
> lands on Korean content.

- [Guide](../guide/server/README.ko.md) — the Spring Boot tutorial angle.
- [Java exact interface](../../common/spec/server/languages/java/interfaces/README.ko.md) —
  the document that owns the contract text itself.
- **This reference** — collects only "what you must know to complete this one call."

## Category

| Category | Status | Corresponding spec |
|---|---|---|
| [Host lifecycle](01-host-lifecycle.en.md) | Drafted | 06-framework-api, 28-graceful-drain-handoff |
| [Topology discovery](02-topology-discovery.en.md) | Drafted | 07-channel-topology, 09-client-server-channel, 10-network-listener-identity, 21-location-runtime |
| [Messaging execution](03-messaging-execution.en.md) | Drafted | 04-message-model, 05-async-execution-policy, 08-channel-messaging |
| [Spot instance](04-spot-instance.en.md) | Drafted | 12-spot-messaging, 15-spot-actor, 16-spot-address-messaging, 17-stage-wrapper-on-spot |
| [Actor relocation](05-actor-relocation.en.md) | Drafted | 14-actor-model, 15-spot-actor, 20-session-actor-dispatch, 28-graceful-drain-handoff |
| [Stream session](06-stream-session.en.md) | Drafted | 19-stream-session, 20-session-actor-dispatch |
| [Location authority](07-location-authority.en.md) | Drafted | 21-location-runtime, 22-location-store-redis, 23-relocation-store-redis, 28-graceful-drain-handoff |
| [Observability diagnostics](08-observability-diagnostics.en.md) | Drafted | 24-runtime-monitoring, 25-runtime-metrics, 26-message-flow-tracing, 27-flow-correlation, 29-transport-liveness |

**Java-specific notation.** The Java framework wires up as a Spring Boot starter — registration-
time calls are `@EnableZLinkFramework` and the `ZLinkFrameworkConfigurer` bean, and runtime
client/runtime access uses beans Spring injects (`ZLinkFrameworkRuntime`, `ZLinkClient`, etc.).
The Kotlin framework reuses this Java surface as-is and does not have a separate reference tree
(only its guide covers Kotlin-specific idiom).

ko and en are both complete. This document tree is wired into `mkdocs.yml` nav.
