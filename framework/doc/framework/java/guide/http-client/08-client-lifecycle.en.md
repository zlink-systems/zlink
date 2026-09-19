---
title: "Client and Request Lifecycle · Java"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/http-client/08-client-lifecycle.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Client and Request Lifecycle

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Streaming](07-streaming.en.md) | [Next: Redirect, Retry, and Cookies](09-redirect-retry-cookie.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/http-client/08-client-lifecycle.en.md) · [C#/.NET](../../../dotnet/guide/http-client/08-client-lifecycle.en.md) · **Java** · [Kotlin](../../../kotlin/guide/http-client/08-client-lifecycle.en.md) · [Node/TypeScript](../../../node/guide/http-client/08-client-lifecycle.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    This chapter establishes when HTTP call resources are reused and closed, how a request finishes, and where timeouts and options apply. Its creation code comes from each language's `HttpClient` tutorial.

A client is a reusable unit that holds a base URL and transport options. A builder creates the client; a request builder gathers an HTTP method, path, and body. A terminator is the final call that submits the gathered request and selects its response form.

## 1. Flow from Builder to Response

<iframe class="zlink-diagram" src="/common/diagrams/http-client-client-lifecycle-en.html"
        title="http client client lifecycle" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-client-lifecycle-en.html" target="_blank">↗ 크게 보기</a></p>

The client keeps the connection pool and defaults for requests to the same target service. A request builder belongs to one request, and its terminator submits that request.

## 2. One Client Is Reused per Service

One client per service is reused for multiple requests. Building a client per request recreates keep-alive and the connection pool. The language's resource-management mechanism closes the client when it is no longer needed.

```java
--8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-client-create"
```

The example creates one client with a base URL and a three-second timeout, then closes it at the end of the program lifetime.

A Spot or channel handler uses a DI-provided client instead of creating one through the static factory. This boundary preserves connection-pool reuse across handlers and the framework execution context.

## 3. Each Response Form Has a Terminator

A terminator submits the request builder as an HTTP request. A typed response carries status, headers, and a JSON-decoded body; a raw response carries status, headers, and the body before JSON decoding. The same response form has different language spellings, and the names in the table identify the response role rather than the execution model of the calling site.

| Response form | C++ | C#/.NET | Java | Kotlin | Node/TypeScript |
| --- | --- | --- | --- | --- | --- |
| typed response `HttpResponse<T>` | `async<T>()` | `Async<T>(ct?)` | `submit(Class<T>)` | `await(type)` / `await<T>()` (suspend) | `submit<T>()` |
| raw response | `async_raw()` | `AsyncRaw(ct?)` | `submitRaw()` | `awaitRaw()` (suspend) | `submitRaw()` |
| decoded body `T` only | `fetch<T>()` | `Fetch<T>(ct?)` | `fetch(Class<T>)` | `fetch<T>()` (suspend) | `fetch<T>()` |
| streaming download | `download(sink)` | `DownloadAsync(sink, ct?)` | `download(Consumer<byte[]>)` | `awaitDownload(sink)` | `download(sink)` |
| callback completion | `async<T>(callback)` | `Async<T>(callback)` | `submit(Class<T>, callback)` | replaced by a suspend function | `submit<T>(callback)` |
| gate return (DI server builder only) | `yield<T>()` | `Yield<T>(ct?)` | `yield(Class<T>)` | `yield<T>()` (suspend) | `yield<T>()` |
| blocking (CLI and client scenarios only) | `submit_raw()`, `submit<T>()` | unavailable | unavailable | unavailable | unavailable |

## 4. Distinguish the Two Timeout Boundaries

The client `timeout` defaults to 3000ms; a request builder `timeout` changes the per-attempt timeout only for that request. A request with retry enabled applies this timeout to every attempt. There is no option that sets one deadline over all retries.

## 5. Asynchronous Completion Does Not Occupy the Caller

An asynchronous terminator does not occupy the caller thread or event loop while the network is pending. The language's asynchronous composition mechanism waits for completion. A blocking terminator exists only for C++ CLI and client scenarios; it cannot be used in a framework runtime execution context.

## 6. Client Options Have One Configuration Point

| Option | Default | Meaning |
| --- | --- | --- |
| `baseUrl` | required | base URL for every request path |
| `timeout` | 3000ms | per-attempt timeout |
| `defaultHeader` | none, accumulated | default header attached to every request |
| `basicAuth` | off | Basic authentication |
| `bearerToken` | off | Bearer authentication |
| `maxResponseBodySize` | 16 MiB | body limit applied after decompression too |
| `trustCertificateFile` | system roots | adds a trusted PEM certificate |
| `clientCertificateFile` | off | mTLS client certificate and key |
| `followRedirects` | off; 5 when called without an argument | automatic redirect limit |
| `retry` | off; 0 attempts | additional retry attempts |
| `cookies` | off | enables the cookie jar |
| `proxy` | off | `http://` proxy URL |
| `proxyBasicAuth` | off | proxy Basic authentication |
| `compression` | off | requests gzip/deflate and decodes transparently |

[Authentication, TLS, and Proxy](06-auth-tls-proxy.en.md) covers authentication and proxy options, [Streaming](07-streaming.en.md) covers streaming exceptions, and [Redirect, Retry, and Cookie](09-redirect-retry-cookie.en.md) covers redirect and retry rules.

## 7. A One-Shot Is a Single Convenience Path

A client builder can start an HTTP method directly. This one-shot path creates its client when submitted and closes it after completion, so it does not reuse the connection pool. A one-shot request cannot be submitted again; resubmission fails with `InvalidOperation`. It fits a single administrative operation, while repeated and high-load calls use a reusable client.

## 8. Next Chapter

[Redirect, Retry, and Cookie](09-redirect-retry-cookie.en.md) covers automatic redirect, retry, and cookie-jar rules.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
