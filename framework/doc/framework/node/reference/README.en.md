# Node.js Framework Reference

The writing rules follow the
[Reference-writing guide](../../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This tree reuses the same 8 categories and order as the dotnet reference (the
parity-reference lane), and each entry was written by cross-checking the Node.js exact interface
(the TypeScript declaration) directly.

> The exact-interface document linked below stays Korean-only. Following that link lands on
> Korean content.

- [Node.js exact interface](../../common/spec/server/languages/node/interfaces/README.ko.md) —
  the document that owns the contract text itself.
- **This reference** — collects only "what you must know to complete this one call."

## Category

| Category | Status | Corresponding exact interface |
|---|---|---|
| [Host lifecycle](01-host-lifecycle.en.md) | Drafted | 03-location-observability §4, 07-nestjs-host |
| [Topology discovery](02-topology-discovery.en.md) | Drafted | 01-foundation-configuration, 07-nestjs-host, 03-location-observability §5-6 |
| [Messaging execution](03-messaging-execution.en.md) | Drafted | 02-channel-messaging |
| [Spot instance](04-spot-instance.en.md) | Drafted | 04-spots, 06-stream-worker, 07-nestjs-host |
| [Actor relocation](05-actor-relocation.en.md) | Drafted | 05-actors |
| [Stream session](06-stream-session.en.md) | Drafted | 02-channel-messaging §6, 06-stream-worker |
| [Location authority](07-location-authority.en.md) | Drafted | 08-location-maintenance, 03-location-observability §2, 07-nestjs-host |
| [Observability diagnostics](08-observability-diagnostics.en.md) | Drafted | 01-foundation-configuration §3, 03-location-observability §1·§3 |

**Node.js-specific notation.** The Node framework splits between
`@zlink-systems/framework` (raw builders/clients) and `@zlink-systems/nestjs` (NestJS
`DynamicModule`, decorators, DI tokens). This reference is written around the NestJS surface
(`zlinkFramework()` builder, `@zlinkXxxHandler` decorators, `ZLINK_*` DI tokens) that real
applications mostly encounter, and raw clients (`ZLinkRouteClient`, `ZLinkActorManager`, etc.) are
injected via their DI token and called directly.

ko and en are both complete. This document tree is wired into `mkdocs.yml` nav.
