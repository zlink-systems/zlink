# 11. Regression Test Contract

> [Common contract table of contents](README.en.md)

Each language verifies the **common contract case matrix** below with
a contract test based on a real server (an in-process HTTP/TLS test
server). This document is canonical for the case specification, and the
per-language test file is the implementation.

## 11.1 Test Location

| Language | Contract Test | Method |
| --- | --- | --- |
| cpp | `http-client/tests/test_cpp_http_client.cpp` | gtest + in-process Beast server (including TLS) |
| dotnet | `tests/Zlink.HttpClient.UnitTests/HttpClientContractTests.cs` (+`RuntimeUnitTests.cs`) | xunit + `HttpListener` server |
| java | `zlink-http-client/src/test/.../HttpClientContractTest.java` (+`CookieJarTest`) | JUnit + `com.sun.net.httpserver` |
| kotlin | `zlink-http-client-kotlin/src/test/.../HttpClientCoroutineTest.kt` | Only suspend bridge/DSL/deserialization (java is responsible for transport semantics) |
| node | `test/contract/http-client.test.js` | `node:test` + `node:http/https/net` server |

## 11.2 Common Case Matrix

A new language/new feature judges omission against this matrix as the
standard.

**Request assembly**: 7-verb dispatch · path `/` validation · query
percent-encoding · default header + per-request override ·
Basic/Bearer injection.

**body**: typed JSON round trip · raw · form · multipart · body source
exclusion violation → `ProtocolError` · whether streaming upload is
actually chunked (raw socket verification).

**Response**: HEAD empty body · typed null body for 204/empty success ·
typed status ≥ 400 → `InternalFailure` · malformed JSON →
`ProtocolError` · `maxResponseBodySize` enforcement.

**redirect**: 303 POST→GET rewrite · same-origin `Authorization`
preservation · cross-origin removal · bound exceeded · relative/
absolute Location.

**retry/timeout**: retry succeeds after a transport failure · retry
exhaustion · timeout → auto-retry target · streaming excluded from
retry.

**cookie**: store/send round trip · Path scope match · Secure not sent
over http · `Max-Age<=0` deletion · 128-per-host eviction (currently
unverified for cpp — a gap).

**Compression**: gzip/deflate transparent decompression · corrupted
body → `ProtocolError` · post-decompression size bound.

**TLS/proxy**: success with a trust certificate · untrusted rejection ·
hostname mismatch rejection · mTLS presentation · proxy plaintext/
CONNECT/auth (per-language possible scope).

**Execution model**: 20 concurrent requests aren't serialized (proof
of non-blocking) · one-shot path works · (cpp) custom execute/resume
scheduler.

## 11.3 Gate

- Coverage: java/kotlin JaCoCo LINE ≥ 0.80, node's built-in coverage
  gate 80%. For dotnet/cpp, the gate is the whole contract test suite
  green.
- Cross-language verification: node's `verify:cross-language` gate
  exists. Once the common spec is confirmed, it's expanded to a
  5-language matrix cross-check gate (tracked in the plan).

## 11.4 Known Coverage Gaps (Tracked In The Plan)

- cpp: pool eviction/TTL, cookie 128 eviction, IPv6 URL, default
  scheduler serialization/deadlock, gzip header parser fuzz.
- dotnet: explicit caller-cancellation, 307/308 body preservation on
  retry.
- Common: multi-thread concurrency stress (cookie jar/pool).
