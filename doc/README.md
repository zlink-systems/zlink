English | [한국어](README.ko.md)

# zlink Documentation

> zlink project documentation navigation

## Directory Purpose

| Directory | Audience | Purpose |
|-----------|----------|---------|
| `spec/` | Binding developers, API reviewers | **Public API contract** — function signatures, return values, error codes, ownership rules. Source of truth: `core/include/zlink.h` |
| `guide/` | Application developers (library users) | **Intent, purpose, usage** — why the API exists, when to use which pattern, practical examples. No internal implementation details |
| `internals/` | Core library maintainers | **Internal architecture** — socket wiring, data flow, thread model, protocol encoding. Diagram-heavy for understanding before reading code |
| `building/` | Build/release engineers | Build, test, packaging instructions |
| `license/` | Anyone evaluating adoption, legal reviewers | **License policy** — why this repo uses three licenses (MPL-2.0 / FSL-1.1-ALv2 / Apache-2.0), what each permits, where the canonical texts live |

**Key rule**: guide does not explain internals. If a guide topic needs
internal context, link to the internals document instead.

---

## Paths by Audience

| Audience | Starting Document | Description |
|----------|-------------------|-------------|
| **Library Users** | [guide/01-overview.md](../core/doc/guide/01-overview.en.md) | Developing messaging applications with the zlink API |
| **Binding Users** | [guide/bindings/README.ko.md](../bindings/doc/guide/README.ko.md) (Korean) | C++/Java/.NET/Node.js/Python bindings |
| **Library Developers** | [internals/architecture.md](../core/doc/internals/architecture.en.md) | Internal architecture and implementation details |
| **Build/Release Engineers** | [building/build-guide.md](building/build-guide.md) | Building, testing, and packaging |
| **Adopters / Legal Reviewers** | [license/README.md](license/README.md) | License policy across `core`/`bindings`/`framework`/`http-client` |

---

## User Guide (guide/)

### Core
| Document | Description |
|----------|-------------|
| [01-overview.md](../core/doc/guide/01-overview.en.md) | zlink overview and getting started |
| [02-core-api.md](../core/doc/guide/02-core-api.en.md) | Core C API detailed guide |
| [03-0-socket-patterns.md](../core/doc/guide/03-0-socket-patterns.en.md) | Socket patterns overview and selection guide |
| [03-1-pair.md](../core/doc/guide/03-1-pair.en.md) | PAIR socket (1:1 bidirectional) |
| [03-2-pubsub.md](../core/doc/guide/03-2-pubsub.en.md) | PUB/SUB/XPUB/XSUB publish-subscribe |
| [03-3-dealer.md](../core/doc/guide/03-3-dealer.en.md) | DEALER socket (asynchronous requests) |
| [03-4-router.md](../core/doc/guide/03-4-router.en.md) | ROUTER socket (ID-based routing) |
| [03-5-stream.md](../core/doc/guide/03-5-stream.en.md) | STREAM socket (RAW communication) |
| [04-transports.md](../core/doc/guide/04-transports.en.md) | Transport guide (tcp/ipc/inproc/ws/wss/tls) |
| [05-tls-security.md](../core/doc/guide/05-tls-security.en.md) | TLS/SSL configuration and security guide |
| [06-monitoring.md](../core/doc/guide/06-monitoring.en.md) | Monitoring API usage |

### Services
| Document | Description |
|----------|-------------|
| [Framework service overview](../framework/doc/framework/common/guide/server/03-concepts.ko.md) | Framework service and object model overview |

### Reference
| Document | Description |
|----------|-------------|
| [08-routing-id.md](../core/doc/guide/08-routing-id.en.md) | Routing ID concepts and usage |
| [09-message-api.md](../core/doc/guide/09-message-api.en.md) | Message API details |
| [10-performance.md](../core/doc/guide/10-performance.en.md) | Performance characteristics and tuning guide |

## Library Specification (spec/)

| Document | Description |
|----------|-------------|
| [spec/README.md](../core/doc/spec/README.en.md) | Specification master index |
| [spec/core/README.md](../core/doc/spec/core/README.en.md) | Core C library specification |
| [spec/core/socket/](../core/doc/spec/core/socket/README.en.md) | Socket specifications (common + per-type) |
| [spec/bindings/README.md](../bindings/doc/spec/README.en.md) | Cross-language binding policy and per-language specs |

## Binding Guides (guide/bindings/)

Per-language usage guides live under
[`guide/bindings/`](../bindings/doc/guide/README.ko.md). Messaging concepts are owned by the
core guide; each language guide covers that language's usage, type mapping, and
language-specific rules.

| Language | Guide | Notes |
|----------|-------|-------|
| .NET | [dotnet/](../bindings/doc/guide/dotnet/index.ko.md) (Korean) | LibraryImport, .NET 8+ |
| C++ / Java / Node / Python / Go / Rust | [Binding guide index](../bindings/doc/guide/README.ko.md) | planned |

## Internals (internals/)

| Document | Description |
|----------|-------------|
| [architecture.md](../core/doc/internals/architecture.en.md) | System architecture overview (5-layer details) |
| [protocol-zmp.md](../core/doc/internals/protocol-zmp.en.md) | ZMP v1.0 protocol details |
| [protocol-raw.md](../core/doc/internals/protocol-raw.en.md) | RAW (STREAM) protocol details |
| [stream-socket.md](../core/doc/internals/stream-socket.en.md) | STREAM socket internals, WS/WSS optimization, runtime defaults |
| [socket-option-defaults.md](../core/doc/internals/socket-option-defaults.en.md) | Effective socket option defaults from code |
| [threading-model.md](../core/doc/internals/threading-model.en.md) | Threading and concurrency model |
| [Framework internals](../framework/doc/framework/common/internals/README.ko.md) | Framework runtime structure and ownership boundaries |
| [design-decisions.md](../core/doc/internals/design-decisions.en.md) | Design decision records |

## Build and Development (building/)

| Document | Description |
|----------|-------------|
| [build-guide.md](building/build-guide.md) | Build instructions (CMake, per-platform) |
| [cmake-options.md](building/cmake-options.md) | CMake options reference |
| [packaging.md](building/packaging.md) | Release and packaging |
| [release-accounts.md](building/release-accounts.md) | Official distribution accounts/secrets |
| [../core/tests/README.md](../core/tests/README.md) | Test strategy, layout, and lane execution |
| [platforms.md](building/platforms.md) | Supported platforms and compilers |

## License Policy (license/)

| Document | Description |
|----------|-------------|
| [README.md](license/README.md) | Why this repository uses three licenses (MPL-2.0 / FSL-1.1-ALv2 / Apache-2.0), what each tier permits, and where the canonical texts live |
