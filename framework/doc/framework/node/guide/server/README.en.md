---
title: "Guide Home · Node/TypeScript"
---

# ZLink Framework Node.js — User Guide

The order to use ZLink Framework in a Node.js/NestJS environment. Chapters 03–17 share the
same source across every language, and the example switches to Node.js code when you pick
the `.ts` tab.

| Order | Document | Content |
|----|------|------|
| 1 | [1. Overview](01-overview.en.md) | What it builds, the four integration axes, and the overall topology |
| 2 | [2. Getting Started](02-getting-started.en.md) | The package, the NestJS module, the handler, token injection |
| 3 | [3. Core Concepts](03-concepts.en.md) | Channel · Spot · Actor · session · relocation |
| 4 | [4. Backpressure](04-backpressure.en.md) | How the system behaves when arrival outpaces processing, and the options that affect it |
| 5 | [5. Channel Messaging](05-channel-messaging.en.md) | Registering and calling request / send / pub-sub |
| 6 | [6. Spot](06-spot.en.md) | A dynamic state unit such as a room · stage · zone |
| 7 | [7. Actor And Spot](07-actor-spot.en.md) | Actor hosting, membership, relocation |
| 8 | [8. Session And Actor Binding](08-actor-session.en.md) | Session ↔ Actor relay · binding · push |
| 9 | [9. STREAM](09-stream.en.md) | External-client real-time connections and the Stream Connector |
| 10 | [10. Location](10-location.en.md) | Registering a location store, auto-connect, operational queries |
| 11 | [11. Monitoring](11-monitoring.en.md) | State snapshots · async iterables · diagnostics |
| 12 | [12. Operations](12-operations.en.md) | Runtime metrics, graceful drain, readiness |
| 13 | [13. Key Type Usage Index](13-interface-catalog.en.md) | Injection tokens, decorators, contract types |
| 14 | [14. Picking A Sample](14-samples.en.md) | How to choose which sample to look at first and run it |
| 15 | [15. E2E Testing](15-e2e-testing.en.md) | How to verify the whole system with the client |
| 16 | [16. Options](16-options.en.md) | The option list, defaults, and when they can change |
| 17 | [17. Where ZLink Fits](17-alternative.en.md) | Where it's used, the warning signs, and the boundary of the technology choice |

The file number identifies the same chapter regardless of language. This table owns the
reading order.

Chapters 01, 02, 11, 13, and 16 are written separately for Node.js because the install steps
and surface names differ per language. Open each chapter directly from the links in the table
above.

## Related Documents

- Public contract: [Node.js public contract](../../../common/spec/server/languages/node/README.ko.md)
- Language-neutral meaning: [Common spec](../../../common/README.ko.md)
- Client library: [HTTP client](../http-client/README.ko.md) · [Stream connector](../stream-connector/README.ko.md)
