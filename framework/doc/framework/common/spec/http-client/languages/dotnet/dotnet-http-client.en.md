# Spec -- ZLink HTTP Client For .NET

> See the [user guide](../../../../../dotnet/guide/http-client/README.en.md)
> for a usage-focused document.
> **The language-neutral common contract is owned by the
> [common spec](../../README.en.md)**, and this document only
> describes the .NET-specific deviation and implementation mapping for
> the common contract.
> The public contract's standard is the common spec and this
> per-language spec. The public types under `src/Zlink.HttpClient/**`
> and the `Zlink.HttpClient.UnitTests` regression test verify whether
> the implementation keeps that contract.

## 1. Purpose

`Zlink.HttpClient` is a separate client-side deliverable for sending an
HTTP request in .NET. It's not a JSON-only client — it's a
general-purpose HTTP client that absorbs `System.Net.Http`'s low-level
configuration in zlink fluent builder style. The typed path
(`Body(dto)`/`Async<T>()`/`Fetch<T>()`) is a convenience layer laid on
top of it.

This client depends on the package-neutral shared artifact
`Zlink.Framework.Contracts`'s error model (`ZLinkFrameworkException`)
and codec contract, but doesn't depend on `Zlink.Framework` server
runtime. So a standalone client uses the same error/codec contract
without distributing the server runtime assembly together.

The HTTP codec registry only handles serializer registration. Even if
the same extension also implements `IZlinkStreamCodecRegistration`, it
ignores the STREAM descriptor. The HTTP client package doesn't depend
on the Stream Connector runtime or the compression package.

## 2. Deliverable Boundary

| Role | Location | Public? |
|------|------|-----------|
| Public contract | `src/Zlink.HttpClient/*.cs`, `Contracts/*` | public |
| Shared error/codec contract | `src/Zlink.Framework.Contracts/Codecs`, `Errors` | public dependency |
| Runtime implementation | `src/Zlink.HttpClient/Runtime/*` | internal |
| Regression test | `tests/Zlink.HttpClient.UnitTests/*` | private |
| Project | `Zlink.HttpClient` | public package |

The public surface doesn't expose `System.Net.Http` types such as
`SocketsHttpHandler`, `HttpClientHandler`, `HttpRequestMessage`,
`HttpResponseMessage`.

## 3. Public Types

- `ZLinkHttpClient` — a standalone client built from a static factory.
  Provides `Get/Post/Put/Delete/Patch/Head/Options`, `IDisposable`.
- `ZLinkHttpServerClient` — the client the framework server provides
  through DI. Each verb returns a `ZLinkHttpServerRequestBuilder`.
- `ZLinkHttpClientBuilder` — `BaseUrl`, `Codecs`, `Timeout`,
  `DefaultHeader`, `BasicAuth`, `BearerToken`, `MaxResponseBodySize`,
  `TrustCertificateFile`, `ClientCertificateFile`, `FollowRedirects`,
  `Retry`, `Cookies`, `Proxy`, `ProxyBasicAuth`, `Compression`,
  `Build`, `BuildServer`, and a one-shot verb shortcut.
  (`Codecs` is framework codec extension registration — a .NET-specific
  extension point, a language deviation of
  [common spec Chapter 2.3](../../02-client-builder.en.md))
- `ZLinkHttpRequestBuilder` — the standalone surface. Provides
  `Header`, `Query`, `Timeout`, `Body<T>`, `Body(content, contentType)`,
  `BodyStream`, `Form`, `Multipart`, `MultipartFile`, `AsyncRaw`,
  `DownloadAsync`, `Async<T>`, `Fetch<T>` (directly returns the decoded
  body), and a callback overload.
- `ZLinkHttpServerRequestBuilder` — includes the standalone surface and
  adds the one-way `ValueTask Async(CancellationToken cancellationToken = default)`.
  The returned `ValueTask` only delivers async completion and failure,
  not the transport result or admission status. It also adds
  `ValueTask<HttpResponse<T>> Yield<T>(CancellationToken cancellationToken = default)`,
  which returns the shared Spot gate and picks it back up on a new
  turn. Used only in a `SpotWide` User Spot or Instance Spot, where
  gate return is allowed.
- `IZLinkHttpExecutionScheduler` / `IZLinkHttpExecutionTurn` — a public
  injection point where the DI integration captures the current Spot
  turn and places callback completion on the original execution
  queue's new turn.
- `RawHttpResponse` { `Status`, `Headers`, `Body` }.
- `HttpResponse<T>` { `Status`, `Headers`, `Body`, `RawBody` }.
- `ZLinkHttpMethod` enum.

The delegate the callback overload receives is also included in the
public contract. On completion, exactly one of `error` and `response`
is non-null.

```csharp
public delegate void ZLinkHttpCallback<T>(
    Exception? error,
    HttpResponse<T>? response);
```

## 4. Execution Model

- `Async<T>` returns `ValueTask<HttpResponse<T>>` and keeps the Spot
  turn.
- `Fetch<T>` returns `ValueTask<T>`. An application sample that doesn't
  need status and header uses this terminal instead of pulling `.Body`
  after receiving the response.

```csharp
public ValueTask<T> Fetch<T>(
    CancellationToken cancellationToken = default);
```
- The HTTP request builder doesn't provide `Yield<T>`. To return the
  shared Spot gate, call `Async<T>` inside `RunIoWorker(...)` and wait
  with the Worker call's `Yield`.
- A callback overload doesn't return an awaitable. The completion
  callback is placed as a new turn on the execution queue of the Spot
  turn that made the request. In a standalone client, it's called
  directly in an async completion context.
- A blocking terminator that pulls the completion value synchronously
  isn't provided.

## 5. Transport Semantics

Default value/redirect/retry/cookie/compression/auth-scrubbing/body-
source-exclusion semantics follow the
[common spec Chapters 2-8](../../README.en.md). .NET implementation
mapping:

- Native automatic feature disabled: `AllowAutoRedirect=false`,
  `AutomaticDecompression=None`, `UseCookies=false` on
  `SocketsHttpHandler` — semantics implemented by the wrapper.
- Per-attempt timeout is enforced with a linked
  `CancellationTokenSource.CancelAfter` instead of `HttpClient.Timeout`
  (distinguishing caller cancellation from timeout).
- TLS: `SslClientAuthenticationOptions` (trust addition + mTLS). proxy:
  `WebProxy`.
- Decompression: performed by the wrapper with
  `System.IO.Compression`.

## 6. Error Mapping

Follows [common spec Chapter 9](../../09-error-model.en.md). .NET uses
`ZLinkFrameworkErrorKind`, and the public exception doesn't provide a
retry indicator.

- Timeout is reported as `DeadlineExceeded` with an inner
  `TimeoutException`. Caller cancellation propagates as
  `OperationCanceledException` as is.

## 7. Regression Test Axis

`Zlink.HttpClient.UnitTests`'s `HttpClientContractTests` verifies the
transport contract scenario. Chunked upload is verified with a
raw-socket server (`RawCaptureServer`), since the managed Linux
`HttpListener` can't receive a chunked request body.
