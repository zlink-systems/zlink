[한국어](README.ko.md) | English

[Specification index](../README.en.md)

# ZLink Core specification

This index links the Core public C ABI contract exposed by `zlink.h`. Formal API documents describe only public contracts; they do not describe source directories, socket wiring, or queue structure.

## 1. Common contracts

| Document | Content |
|---|---|
| [Public-contract governance](00-public-contract-governance.en.md) | Consistency among specification, headers, tests, and packages |
| [Context](01-context.en.md) | Context creation, shutdown, and configuration |
| [Message](02-message.en.md) | Message lifecycle, routing IDs, and ownership |
| [Errors](03-errors.en.md) | Public result enums, errno, and version |
| [Errno map](04-errno-map.en.md) | Result and errno mappings by API family |
| [Events](05-events.en.md) | Common event types and readiness meaning |
| [Polling](06-polling.en.md) | Poll items, pollers, and source support |
| [Monitoring](07-monitoring.en.md) | Raw-socket monitors and status snapshots |
| [Utilities](08-utilities.en.md) | Timers, threads, stopwatch, and atomic helpers |
| [Runtime boundary](09-runtime-boundary.en.md) | Core raw C ABI and Framework service responsibility boundary |

## 2. Socket contracts

| Document | Content |
|---|---|
| [Socket index](socket/README.en.md) | Common lifecycle, options, send, and receive |
| [PAIR](socket/01-pair.en.md) | One-to-one connection |
| [PUB](socket/02-pub.en.md) | Classic fanout publisher |
| [SUB](socket/03-sub.en.md) | Classic fanout subscriber |
| [XPUB](socket/04-xpub.en.md) | Subscription-aware publisher |
| [XSUB](socket/05-xsub.en.md) | Upstream subscription socket |
| [DEALER](socket/06-dealer.en.md) | Asynchronous request source |
| [ROUTER](socket/07-router.en.md) | Raw routing-ID router |
| [STREAM](socket/08-stream.en.md) | Raw TCP or WebSocket session socket |
