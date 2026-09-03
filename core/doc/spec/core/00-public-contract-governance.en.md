---
title: "Public Contract Governance"
---

[한국어](https://zlink-systems.github.io/zlink/ko/spec/core/00-public-contract-governance/) | English

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Overview](README.en.md) | [Next: Context](01-context.en.md)
<!-- zlink-nav:end -->

# Public Contract Governance

> **What this chapter defines** — the procedure for keeping specifications, headers, tests, and
> packages aligned.

## 1. Public contract governance overview

This document defines the sources, documentation responsibilities, and change procedure for the
zlink Core public contract. Its audience is developers who design, implement, and review the Core
public C ABI — the binary interface of C functions and types that applications link and call
directly.

The related contracts are owned by the following documents.

| Related contract | Owning document |
|---|---|
| Exact responsibility boundary of the Core public C ABI | [Core Runtime Boundary](08-runtime-boundary.en.md) |

## 2. Contract sources

The formal specifications under `core/doc/spec/core/` define the target public behavior that Core
must provide. `core/include/zlink.h` and the domain headers it includes are the C ABI expression of
the same contract. The implementation, contract tests that verify public behavior through
observable results, bindings that wrap and expose the C ABI in each language, and installation
packages must agree with both expressions.

The Core public C ABI provides only [context](glossary.en.md#context), message,
[socket](glossary.en.md#socket), transport, eventing, and utility capabilities used directly by
applications and raw bindings. It provides no separate public SPI (service provider interface that
lets Framework supply an implementation), private C ABI, or compatibility facade for the Framework
service runtime. [Core Runtime Boundary](08-runtime-boundary.en.md) owns the exact responsibility
boundary.

The formal specifications are not reduced for implementation convenience. A mismatch between the
headers and specifications prevents the implementation from being considered complete, and neither
is retained as a separate compatibility layer.

## 3. Documentation responsibilities

Each document type owns the following content.

| Document type | Owned content |
|---|---|
| Formal specification | function signatures, types and constants, results and errno, ownership, timeout, thread safety, callback semantics, and close semantics |
| Guide | usage purpose and application examples |
| Internals | socket wiring, queues, locks, threads, and protocol codecs — described after the implementation is confirmed |

Formal specifications, guides, and internals describe only the Core target state. Plans and
execution ledgers own implementation progress and alternatives; an execution ledger records work
progress. Formal documents do not cite plans as contract authority.

## 4. Core target-contract-first exception

The Core service-runtime migration redefines the responsibility boundary for a major version. For
this work, the formal specifications establish the target contract before the public headers and
current implementation. Even an unimplemented target is written in the present tense in a formal
specification rather than in a draft, and implementation differences are recorded only in the v11
execution ledger.

This exception applies only to the Core migration scope. Other Core changes follow the repository's
general draft procedure.

## 5. Change procedure

A Core public contract change follows this order.

1. Record the target contract with the same meaning in the Korean and English formal specifications.
2. Review functions, types, enums, constants, results, ownership, and thread-safety tables.
3. Align public headers, exported symbols, and implementation with the formal specifications.
4. Verify the raw public API with contract tests and bindings-package snapshots.
5. Align internals with the surviving structure after the actual implementation and structural tests are confirmed.
6. Reverify parity among the Korean and English documents, headers, and packages.

## 6. Korean and English parity

The Korean and English documents must have the same heading order, C signatures, enum and struct
fields and numeric values, defaults, results, ownership, thread-safety rules, and link targets. A
public contract present in only one language is not recognized as a valid Core contract.

<!-- zlink-nav:start -->
[Core Spec Index](README.en.md) | [Previous: Overview](README.en.md) | [Next: Context](01-context.en.md)
<!-- zlink-nav:end -->
