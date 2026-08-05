# Spec -- ZLink HTTP Client For Node

> See the [user guide](../../../../../node/guide/http-client/README.en.md)
> for a usage-focused document.
> **The language-neutral common contract is owned by the
> [common spec](../../README.en.md)**, and this document only
> describes the Node-specific deviation and implementation mapping for
> the common contract.
> The single standard for the actual contract is the common spec +
> `packages/http-client/src/**`'s public types and the
> `test/contract/http-client.test.js` regression test.

## 1. Purpose

`@zlink-systems/http-client` is a separate client-side deliverable for
sending an HTTP request in Node. It's not a JSON-only client — it's a
general-purpose HTTP client that absorbs undici's low-level
configuration in zlink fluent builder style. The typed JSON path
(`body(dto)`/`async<T>()`) is a convenience layer laid on top of it.

It depends on `@zlink-systems/framework`'s error model
(`ZLinkFrameworkException`) but isn't a default dependency of framework
core (one-way dependency).

## 2. Deliverable Boundary

| Role | Location | Public? |
|------|------|-----------|
| Library contract | `packages/http-client/src/{index,client,request-builder,types}.ts` | Package-internal public surface |
| Runtime implementation | `packages/http-client/src/runtime/*` | internal |
| Regression test | `test/contract/http-client.test.js` | private |
| Package | `@zlink-systems/http-client` | Currently workspace-only private package |

The public surface doesn't expose an undici `Dispatcher`/`Agent`/
`request` type.

## 3. Public Types

- `ZLinkHttpClient` — `create()` / `create(baseUrl)`, methods
  `get/post/put/delete/patch/head/options`, `close()`.
- `ZLinkHttpClientBuilder` — `baseUrl`, `timeout`, `defaultHeader`,
  `basicAuth`, `bearerToken`, `maxResponseBodySize`,
  `trustCertificateFile`, `clientCertificateFile`, `followRedirects`,
  `retry`, `cookies`, `proxy`, `proxyBasicAuth`, `compression`,
  `build`, and a one-shot verb shortcut.
- `ZLinkHttpRequestBuilder` — `header`, `query`, `timeout`,
  `body` (JSON/raw overload), `bodyStream`, `form`, `multipart`,
  `multipartFile`, `submitRaw`, `download`, `async<T>`,
  `fetch<T>` (`Promise<T>`, returns only the body).
- `ZLinkHttpServerRequestBuilder` — provides the standalone surface and
  one-way `submit(): Promise<void>`. One-way completion carries no
  transport result or admission status. Node HTTP Client's typed
  response terminal keeps `async<T>(): Promise<HttpResponse<T>>`. This
  is because TypeScript's generic type is erased at runtime, so
  declaring a no-argument `submit<T>()` and one-way `submit()` in the
  same inheritance hierarchy would make the two operations
  indistinguishable.
- `RawHttpResponse` { `status`, `headers`, `body` }.
- `HttpResponse<T>` { `status`, `headers`, `body`, `rawBody` }.
- `ZLinkHttpMethod`, `BodyChunkProvider` (`() => Uint8Array | null`),
  `DownloadSink`.

## 4. Execution Model

- Every submit returns a `Promise`. With undici's libuv async I/O, the
  event loop isn't occupied while waiting on the network.
- Since Node has a single event loop, there's no continuation-resume-
  location injection (no `.coroutines()`).
- Node has no synchronous blocking HTTP access.

## 5. Transport Semantics

Default value/redirect/retry/cookie/compression/auth-scrubbing/body-
source-exclusion semantics follow the
[common spec Chapters 2-8](../../README.en.md). Node implementation
mapping:

- **Backend**: undici's low-level `request` (not `fetch` — it has no
  auto-redirect/decompress/cookie, so the wrapper controls semantics).
- **TLS**: `trustCertificateFile`→`Agent.connect.ca` (added to the
  default root), mTLS→`connect.cert/key`.
- **proxy**: `ProxyAgent` — auth is delivered as `token`, not a header,
  so it isn't exposed to the target in the CONNECT tunnel.
- **Decompression**: `node:zlib`.
- Prototype-pollution defense (removes `__proto__`/`constructor`/
  `prototype`) on typed JSON decode — a Node-specific security rule.

## 6. Error Mapping

Follows [common spec Chapter 9](../../09-error-model.en.md). Node only
uses the Framework common kind.

- Timeout is reported as `DeadlineExceeded`, and the exception's
  `cause` is an `Error` with `name` fixed exactly to `TimeoutError`.

## 7. Regression Test / Registration

- Regression test: `test/contract/http-client.test.js` (node:test).
  Chunked upload/retry is verified with a raw `net` server, and
  TLS/mTLS with `node:https` + `test/fixtures/tls/` certificates.
- Registration: workspace `package.json` (undici runtime dependency),
  root `package-lock.json`, `tsconfig.base.json` paths,
  `tsconfig.build.json` references, ESLint flat config (auto-scoped).
- Coverage: exceeds 80% against node's built-in coverage gate
  (`packages/*/dist`).
