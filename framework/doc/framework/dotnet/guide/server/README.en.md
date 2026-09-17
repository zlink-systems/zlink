---
title: "Guide Home · C#/.NET"
---

# ZLink Framework .NET — User Guide

The order to use ZLink Framework in a `.NET`/`ASP.NET Core` environment. Chapters 03–17
share the same source across every language, and the example switches to `.NET` code when
you pick the `C#/.NET` tab.

| Order | Document | Content |
|----|------|------|
| 1 | [Overview](01-overview.en.md) | What it solves and how it differs from the usual way |
| 2 | [Quickstart](../../quickstart.en.md) | Install, a minimal project where two processes call each other, first-run checks |
| 3 | [Core Concepts](03-concepts.en.md) | What a channel, a Spot, an Actor and a session each are |
| 4 | [Channel Messaging](20-channel-messaging.en.md) | The path that calls by name — registering and calling |
| 5 | [Spot](21-spot.en.md) | Creating and calling a shared place by id |
| 6 | [Actor](22-actor.en.md) | Creating and calling one entity by id |
| 7 | [STREAM](23-stream.en.md) | A client outside the mesh attaching over one connection |
| 8 | [Session and Actor](24-actor-session.en.md) | Binding one connection to one Actor |
| 9 | [Location](25-location.en.md) | Looking up the node something is on by id |
| 10 | [Monitoring](26-monitoring.en.md) | A placeholder in the feature guide — no body yet |
| 11 | [The Execution Model](32-execution-model.en.md) | Two queues, the serialization scope, the turn |
| 12 | [Backpressure](33-backpressure.en.md) | When arrival outruns processing, and the options that affect it |
| 13 | [Activation and Lifetime](34-activation-lifetime.en.md) | Creation time per kind, lifecycle callbacks, injection lifetime |
| 14 | [Actor Membership](35-actor-membership.en.md) | Moving between Spots, reservations and limits |
| 15 | [Timers and Workers](36-timer-worker.en.md) | Periodic execution, running outside the line, giving the turn back |
| 16 | [Relocation](37-relocation.en.md) | What survives a move, the adapter, the unit |
| 17 | [How Channels Work](30-channel-patterns.en.md) | Pattern differences, target selection, pub/sub, connection and discovery |
| 18 | [Handlers and Message Processing](31-handler-dispatch.en.md) | Registration variants, filters, codecs, handler kinds |
| 19 | [How STREAM Works](38-stream-boundary.en.md) | Startup checks, error ownership, reply tokens, execution mode |
| 20 | [How Session Binding Works](39-session-binding.en.md) | How many bindings, route refresh, disconnect, failures |
| 21 | [Where ZLink Applies](17-alternative.en.md) | Where it fits, the signals, the boundary, the license |
| 22 | [Operations and Lifecycle](12-operations.en.md) | Runtime metrics, relocate, drain, readiness wiring |
| 23 | [Options](16-options.en.md) | The option list, the defaults and when to change them |
| 24 | [Picking a Sample](14-samples.en.md) | Choosing which sample to read first and how to run it |
| 25 | [E2E Testing](15-e2e-testing.en.md) | Verifying the whole system with the client library |
| 26 | [Key Type Index](13-interface-catalog.en.md) | The contract interfaces indexed by their verification code |
| 27 | [Monitoring](26-monitoring.en.md) | Awaiting rewrite — status snapshots and diagnostics |

The file number identifies the same chapter regardless of language. This table owns the
reading order.

Chapters 01, 11, 13, and 16 are written separately for `.NET` because the install steps
and surface names differ per language.

## Related Documents

- `.NET` documentation entry point: [ZLink Framework for .NET](../../README.en.md)
- Public contract: [.NET public contract](../../../common/spec/server/languages/dotnet/README.en.md)
- Language-neutral meaning: [Common spec](../../../common/README.en.md)
- Client library: [HTTP client](../http-client/README.en.md) · [Stream connector](../stream-connector/INDEX.en.md)
