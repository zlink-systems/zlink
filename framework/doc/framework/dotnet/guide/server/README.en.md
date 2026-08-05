---
title: "Guide Home · C#/.NET"
---

# ZLink Framework .NET — User Guide

The order to use ZLink Framework in a `.NET`/`ASP.NET Core` environment. Chapters 03–17
share the same source across every language, and the example switches to `.NET` code when
you pick the `C#/.NET` tab.

| Order | Document | Content |
|----|------|------|
| 1 | [1. Overview](01-overview.en.md) | What/why/who it's for, the felt difficulty versus the existing approach, the four axes |
| 2 | [2. Getting Started](02-getting-started.en.md) | NuGet install, a minimal two-process example, the TicTacToe room-creation flow |
| 3 | [3. Core Concepts](03-concepts.en.md) | Core concepts mapped to the common spec |
| 4 | [4. Backpressure](04-backpressure.en.md) | How the system behaves when arrival outpaces processing, and the options that affect it |
| 5 | [5. Channel Messaging](05-channel-messaging.en.md) | How to register and call request / send / pub-sub |
| 6 | [6. Spot](06-spot.en.md) | How to register and call a dynamic SPOT such as a room / stage / zone |
| 7 | [7. Actor And Spot](07-actor-spot.en.md) | The Actor model and Actor hosting on a Spot (lifecycle callbacks/trigger functions, the location axis) |
| 8 | [8. Session And Actor Binding](08-actor-session.en.md) | Session ↔ Actor relay/binding/bound-session push (the binding axis) |
| 9 | [9. STREAM](09-stream.en.md) | How to use the external-client STREAM server and the Stream Connector |
| 10 | [10. Location](10-location.en.md) | How to register a location store, auto-connect, and query for operations |
| 11 | [11. Monitoring](11-monitoring.en.md) | How to observe the runtime through state snapshots, the status stream, and diagnostics |
| 12 | [12. Operations](12-operations.en.md) | Operations — runtime metrics, graceful drain, readiness integration |
| 13 | [13. Key Type Usage Index](13-interface-catalog.en.md) | Every contract interface indexed against its ContractTests verification code |
| 14 | [14. Picking A Sample](14-samples.en.md) | How to choose which sample to look at first and run it |
| 15 | [15. E2E Testing](15-e2e-testing.en.md) | How to build an E2E test that verifies the whole system with the client library |
| 16 | [16. Options](16-options.en.md) | Configuration — the option list, defaults, and when they can change |
| 17 | [17. Where ZLink Fits](17-alternative.en.md) | Where it's used, the warning signs, and the boundary of the technology choice |

The file number identifies the same chapter regardless of language. This table owns the
reading order.

Chapters 01, 02, 11, 13, and 16 are written separately for `.NET` because the install steps
and surface names differ per language.

## Related Documents

- `.NET` documentation entry point: [ZLink Framework for .NET](../../README.en.md)
- Public contract: [.NET public contract](../../../common/spec/server/languages/dotnet/README.ko.md)
- Language-neutral meaning: [Common spec](../../../common/README.ko.md)
- Client library: [HTTP client](../http-client/README.ko.md) · [Stream connector](../stream-connector/INDEX.ko.md)
