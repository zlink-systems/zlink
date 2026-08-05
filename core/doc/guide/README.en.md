[한국어](README.ko.md)

# zlink Core Guide

This guide explains the purpose and usage order of the raw Core C API. The
exact function contract is governed by
[`core/doc/spec/core/`](../spec/core/README.en.md).

## Getting Started

- [01 Overview](01-overview.en.md): runtime scope and socket patterns
- [02 Core C API](02-core-api.en.md): the context, socket, and eventing API
- [03-0 Socket Patterns](03-0-socket-patterns.en.md): choosing a communication style

## Sockets And Transport

- [PAIR](03-1-pair.en.md), [PUB/SUB](03-2-pubsub.en.md),
  [DEALER](03-3-dealer.en.md), [ROUTER](03-4-router.en.md)
- [STREAM](03-5-stream.en.md), [Proxy](03-6-proxy.en.md)
- [Transport](04-transports.en.md), [TLS](05-tls-security.en.md)

## Messages And Operations

- [Routing ID](08-routing-id.en.md)
- [Message API](09-message-api.en.md)
- [Monitoring](06-monitoring.en.md)
- [Performance](10-performance.en.md), [Thread safety](11-thread-safety.en.md),
  [Socket options](12-socket-options.en.md)
- [Reliability](reliability.en.md), [Shared scenarios](scenarios.en.md), [Glossary](glossary.en.md)

Application topology and stateful object usage are explained in the
per-language Framework guides under `framework/doc/`.
