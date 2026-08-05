[한국어](00-public-contract-governance.ko.md) | English

[Specification index](../README.en.md) · [Core index](README.en.md)

# Core public-contract governance

This document defines the sources, documentation responsibilities, and change
procedure for the ZLink Core public contract. Its audience is developers
who design, implement, and review the public Core C ABI.

## 1. Contract sources

The formal specifications under `core/doc/spec/core/` define the target public
behavior that Core must provide. `core/include/zlink.h` and the domain
headers it includes are the C ABI expression of the same contract. The
implementation, contract tests, bindings, and installed packages must agree
with both expressions.

The Core public C ABI provides only context, message, socket, transport,
eventing, and utility capabilities used directly by applications and raw
bindings. It provides no separate public SPI, private C ABI, or compatibility
facade for Framework service runtimes. [Core Runtime Boundary](09-runtime-boundary.en.md)
owns the exact responsibility boundary.

The formal specification is not reduced for implementation convenience. A
mismatch between headers and specification prevents completion, and neither
side is retained as a separate compatibility layer.

## 2. Documentation responsibilities

Formal specifications define function signatures, types and constants, results
and errno, ownership, timeout, thread safety, callbacks, and close semantics.
Guides own purpose and application examples. Internals describe socket wiring,
queues, locks, threads, and protocol codecs after implementation is confirmed.

Formal specifications, guides, and internals describe only the Core target
state. Plans and execution ledgers own implementation progress and
alternatives. Formal documents do not cite plans as contract authority.

## 3. Core target-first exception

The Core service-runtime migration changes the responsibility boundary of
a major version. For this work, formal specifications establish the target
contract before public headers and the current implementation. An unimplemented
target is written in the formal specification, not a draft, and the v11
execution ledger alone records implementation differences.

This exception applies only to the Core migration scope. Other Core changes
follow the repository's general draft procedure.

## 4. Change procedure

A Core public-contract change follows this order:

1. Record the target contract with the same meaning in Korean and English formal specifications.
2. Review functions, types, enums, constants, results, ownership, and thread-safety tables.
3. Align public headers, exported symbols, and implementation with the formal specification.
4. Verify the raw public API with contract tests and bindings-package snapshots.
5. Update internals for the surviving structure after implementation and structural tests are confirmed.
6. Reverify parity among Korean, English, headers, and packages.

## 5. Korean and English parity

The Korean and English documents have the same heading order, C signatures,
enum and struct fields and numeric values, defaults, results, ownership,
thread-safety rules, and link targets. A public contract present in only one
language is not a valid Core contract.
