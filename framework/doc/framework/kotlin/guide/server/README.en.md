---
title: "Guide Home · Kotlin"
---

# ZLink Framework Kotlin — User Guide

The order to use ZLink Framework in a Kotlin/Spring Boot environment. Chapters 03–17 share
the same source across every language, and the example switches to Kotlin code when you pick
the `.kt` tab.

| Order | Document | Content |
|----|------|------|
| 1 | [1. Overview](01-overview.en.md) | What the Kotlin layer adds, the four integration axes, and the overall topology |
| 2 | [2. Getting Started](02-getting-started.en.md) | Dependencies, registration, the suspend handler, two ways to call |
| 3 | [3. Core Concepts](03-concepts.en.md) | Channel · Spot · Actor · session · relocation |
| 4 | [4. Backpressure](04-backpressure.en.md) | How the system behaves when arrival outpaces processing, and the options that affect it |
| 5 | [5. Channel Messaging](05-channel-messaging.en.md) | Registering and calling request / send / pub-sub |
| 6 | [6. Spot](06-spot.en.md) | A dynamic state unit such as a room · stage · zone |
| 7 | [7. Actor And Spot](07-actor-spot.en.md) | Actor hosting, membership, relocation |
| 8 | [8. Session And Actor Binding](08-actor-session.en.md) | Session ↔ Actor relay · binding · push |
| 9 | [9. STREAM](09-stream.en.md) | External-client real-time connections and the Stream Connector |
| 10 | [10. Location](10-location.en.md) | Registering a location store, auto-connect, operational queries |
| 11 | [11. Monitoring](11-monitoring.en.md) | Receiving with a `Flow`, lambda observers |
| 12 | [12. Operations](12-operations.en.md) | Runtime metrics, graceful drain, readiness |
| 13 | [13. Key Type Usage Index](13-interface-catalog.en.md) | The suspend contract, the `.kotlin()` wrapper, extension functions |
| 14 | [14. Picking A Sample](14-samples.en.md) | How to choose which sample to look at first and run it |
| 15 | [15. E2E Testing](15-e2e-testing.en.md) | How to verify the whole system with the client |
| 16 | [16. Options](../../../java/guide/server/16-options.en.md) | The option surface is the same as Java's |
| 17 | [17. Where ZLink Fits](17-alternative.en.md) | Where it's used, the warning signs, and the boundary of the technology choice |

The file number identifies the same chapter regardless of language. This table owns the
reading order.

**Kotlin runs on the Java runtime as-is.** `zlink-framework-kotlin` isn't a separate
implementation — it's a thin layer that adds coroutine idioms. So this guide writes up
**only what differs from Java** and points everything else at the
[Java guide](../../../java/guide/server/README.en.md).

- **01, 02** — the dependency and registration code differs, so these are Kotlin-specific.
- **11, 13, 16** — the surface matches Java; only the idiom differs. This guide writes up
  only the difference and points the rest at the Java chapter.

The goal is not to keep two copies of the same content. When the Java document changes,
Kotlin readers see the same document.

## Related Documents

- Public contract: [Kotlin public contract](../../../common/spec/server/languages/kotlin/README.ko.md)
- Language-neutral meaning: [Common spec](../../../common/README.ko.md)
- Client library: [HTTP client](../http-client/README.ko.md) · [Stream connector](../stream-connector/README.ko.md)
