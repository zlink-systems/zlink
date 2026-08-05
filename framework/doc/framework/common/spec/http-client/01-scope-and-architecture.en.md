# 1. Scope And Architecture

> [Common contract table of contents](README.en.md)

## 1.1 Identity

The zlink HTTP client is a companion client that lets a framework
handler and management tool call HTTP under the same framework
error/codec/execution contract. It hides each language's representative
HTTP transport stack behind a fluent builder, but it isn't a
general-purpose client replacing an ordinary HTTP library. It also
isn't a JSON-only client — the typed JSON path is a framework codec
convenience layer laid on top of the raw HTTP path.

It isn't built from scratch. Transport is delegated to each language's
representative stack, but **the semantics (redirect, retry, cookie,
compression, auth scrubbing) are owned directly by the wrapper**, so
they behave identically across the 5 languages. To this end, the
native stack's automatic redirect, automatic decompression, and
automatic cookie are all turned off, and the wrapper implements them.

| Language | Transport Stack | Deliverable |
| --- | --- | --- |
| cpp | Boost.Beast + Asio (+OpenSSL optional) | `zlink::http_client` (CMake, static) |
| dotnet | `System.Net.Http` + `SocketsHttpHandler` | `Zlink.HttpClient` (NuGet) |
| java | `java.net.http.HttpClient` | `zlink-http-client` (Gradle) |
| kotlin | Reuses the java runtime transitively + coroutine extension | `zlink-http-client-kotlin` (Gradle) |
| node | undici low-level `request` | `@zlink-systems/http-client` (npm) |

## 1.2 Public Surface Rule

- The public contract (contracts) **doesn't expose a transport stack
  type** (Beast/Asio, `SocketsHttpHandler`, `java.net.http.*`, and
  undici types are prohibited).
- The public type name is unified to the `ZLinkHttpClient` /
  `ZLinkHttpClientBuilder` / `ZLinkHttpRequestBuilder` /
  `RawHttpResponse` / `HttpResponse<T>` / `ZLinkHttpMethod` family
  (applying language casing convention, see
  [Per-Language Interface Definition](language-interfaces.en.md)).
- The runtime implementation is confined to a per-language internal
  area (`src/runtime`, `internal/`, `Runtime/`) and can't be reached
  from the public API.

## 1.3 Relationship With Framework — One-Way Dependency

- The HTTP client consumes the framework common contract package's
  **error model** (`ZLinkFrameworkException` family) and **codec
  extension** (typed body serialization). In `.NET`, this
  runtime-independent contract is provided by
  `Zlink.Framework.Contracts`.
- The framework core doesn't depend on the HTTP client. The HTTP
  client is an independent deliverable that can be distributed
  separately without framework runtime, but the error/codec contract
  reuses the framework's (it doesn't build a separate exception
  hierarchy).
- Typed body encode/decode **shares the same codec extension** as
  framework and stream-connector. A raw body doesn't go through the
  extension.

## 1.4 Kotlin's Position

The kotlin deliverable isn't an independent implementation — it's a
**thin idiom layer** on top of the java runtime: it only adds a
suspend bridge, DSL builder, reified generic, and Kotlin data class
deserialization. The verification responsibility for the transport
semantics contract falls to the java contract test.
