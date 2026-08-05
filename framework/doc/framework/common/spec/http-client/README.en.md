# ZLink HTTP Client Common Contract

This document set defines the contract C++, .NET, Java, Kotlin, and
Node.js HTTP clients must provide in common. The per-language document
expresses this common behavior with that language's exact public type
and signature.

[10 Revision Candidates](10-revision-candidates.en.md) isn't yet a
public contract. Only a promoted item is reflected in the matching
contract document and every language's interface.

## Table Of Contents

[12 HTTP Client](12-http-client.en.md) defines the whole boundary for
registering and calling an HTTP client in the Framework. 01-09 each own
the detailed contract of builder, response, execution, auth, and
error, and 11 defines the item an implementation and contract test must
verify.

| Ch. | Document | Content |
| --- | --- | --- |
| **12** | [**HTTP Client (Framework Contract)**](12-http-client.en.md) | **Canonical** — identity, fluent builder, terminator (`submit`/`async`/`yield` + callback), turn seam, DI server surface |
| 1 | [Scope And Architecture](01-scope-and-architecture.en.md) | Identity, deliverable boundary, relationship with framework |
| 2 | [Client Builder Contract](02-client-builder.en.md) | The whole builder option set and **default value table** |
| 3 | [Request Contract](03-request-builder.en.md) | HTTP method, header/query, the 5 body sources, and exclusion rules |
| 4 | [Response Contract](04-response-model.en.md) | raw/typed/download/fetch, the status ≥ 400 policy |
| 5 | [Execution Model](05-execution-model.en.md) | Async contract, the no-blocking rule, client lifetime |
| 6 | [Redirect · Retry · Cookie](06-redirect-retry-cookie.en.md) | Rewrite rule table, retry contract, cookie subset |
| 7 | [Auth · TLS · Proxy](07-auth-tls-proxy.en.md) | Basic/Bearer, PEM trust/mTLS, CONNECT tunnel |
| 8 | [Compression](08-compression.en.md) | gzip/deflate transparent decompression semantics |
| 9 | [Error Model](09-error-model.en.md) | The common error kind set, per-language mapping and implementation gap |
| 10 | [Revision Candidates](10-revision-candidates.en.md) | **Not a contract** — items under review before promotion (R1-R14) |
| 11 | [Regression Test Contract](11-regression-tests.en.md) | Common contract case matrix, gate, coverage gap |
| — | [Per-Language Interface Cross-Reference](language-interfaces.en.md) | **Non-normative** — a cross-reference table viewing the five languages' surfaces side by side. Doesn't fix a contract |

## Per-Language Public API

The exact type and signature of each language is owned by the
following document.

| Language | Document |
|------|------|
| C++ | [languages/cpp](languages/cpp/cpp-http-client.en.md) |
| `.NET` | [languages/dotnet](languages/dotnet/dotnet-http-client.en.md) |
| Java | [languages/java](languages/java/java-http-client.en.md) |
| Kotlin | [languages/kotlin](languages/kotlin/kotlin-http-client.en.md) |
| Node.js | [languages/node](languages/node/node-http-client.en.md) |

## Contract Change Procedure

1. A new behavior/public API is first registered as an R-item in
   [Chapter 10 Revision Candidates](10-revision-candidates.en.md). A
   revision candidate isn't a contract and doesn't become an
   implementation basis.
2. Once promotion is decided, it moves to that chapter's contract body,
   and the 5 languages' implementation, contract test, and per-language
   spec are updated together. Implementing only one language first
   isn't allowed (the repository's public contract parity policy).
3. A language-specific deviation (keyword avoidance `delete_`, kotlin
   DSL, etc.) is recognized only if explicitly stated as a
   "language deviation" section in that chapter of this canonical
   document.

## Related Documents

- Per-language user guide: `framework/doc/framework/<lang>/guide/http-client/`
- [Codec Extension Shared Contract](../06-framework-api.en.md)
- [Common E2E Contract](../../e2e/README.en.md)
