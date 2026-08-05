[한국어](README.ko.md) | English

# ZLink public specification

This directory defines the ZLink public API contract. Its audience is Core and bindings implementers and public-contract reviewers. Formal documents linked from this index are authoritative for function signatures, returns, errors, ownership, and thread safety.

## 1. Document structure

| Area | Document | Description |
|---|---|---|
| Core C ABI | [Core specification](core/README.en.md) | C functions, types, enums, and runtime behavior |
| Bindings | [Bindings specification](../../../bindings/doc/spec/README.en.md) | Language-specific public projections of the Core contract |

Formal specifications describe only the current contract. Guides own purpose and examples, while internals own actual internal structure after implementation is complete. Contract reviewers navigate from this index and the public header.

## 2. Main Core documents

| Document | Public contract |
|---|---|
| [Contract governance](core/00-public-contract-governance.en.md) | Consistency among specification, headers, tests, and packages |
| [Context](core/01-context.en.md) | Context lifecycle and options |
| [Message](core/02-message.en.md) | Message and routing-ID storage and ownership |
| [Socket](core/socket/README.en.md) | Generic socket types and send/receive behavior |
| [Polling](core/06-polling.en.md) | Poll items, pollers, and readiness |
| [Monitoring](core/07-monitoring.en.md) | Raw-socket monitors and snapshots |
| [Runtime boundary](core/09-runtime-boundary.en.md) | Core raw C ABI and Framework service responsibility boundary |
| [Events](core/05-events.en.md) | Public events and state-transition meaning |
| [Errors](core/03-errors.en.md) | Result enums, errno, and the version ABI |
| [Errno map](core/04-errno-map.en.md) | Per-function result and errno mappings |
| [Utilities](core/08-utilities.en.md) | Timers, threads, stopwatch, and atomic utilities |

## 3. Conformance

A conforming implementation provides every function, type, constant, and behavior in the formal documents. A mismatch among public headers, exported symbols, contract tests, bindings, installed packages, and the formal specification is nonconforming. Internal implementation details are not exposed as public contracts, and a language-specific API cannot reduce the shared contract.
