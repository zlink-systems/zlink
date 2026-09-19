---
title: "Redirect, Retry, and Cookies · Java"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/http-client/09-redirect-retry-cookie.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Redirect, Retry, and Cookies

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Client and Request Lifecycle](08-client-lifecycle.en.md) | [Next: Response Rules](10-response-rules.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/http-client/09-redirect-retry-cookie.en.md) · [C#/.NET](../../../dotnet/guide/http-client/09-redirect-retry-cookie.en.md) · **Java** · [Kotlin](../../../kotlin/guide/http-client/09-redirect-retry-cookie.en.md) · [Node/TypeScript](../../../node/guide/http-client/09-redirect-retry-cookie.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can decide how a request changes across redirects, which failures automatic retry covers, and what a cookie jar stores.

An external API can move and a network can fail temporarily. A redirect continues a request at the address supplied by the server; retry starts a failed operation again. Enabling both without their boundaries can change a POST or process the same request more than once.

## 1. A request with redirect tracking

Redirect tracking is off by default. This tutorial uses a client with tracking enabled when it reads the legacy player address.

```java
--8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-redirect"
```

```console
redirect: 200 p1
```

The result shows that the final response after following the legacy-address redirect has status 200 and player id `p1`.

## 2. When a redirect changes a request

Redirects resolve absolute and relative `location` URLs. For 301 and 302, GET and HEAD keep their method and body, while POST becomes GET and loses its body. A 303 always becomes GET. A 307 or 308 keeps the method and body, except that a streaming body cannot be rewound and is dropped.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-redirect-retry-en.html"
        title="http client redirect retry" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-redirect-retry-en.html" target="_blank">↗ 크게 보기</a></p>

`Authorization` is removed when the destination has a different origin. An origin is the same only when scheme, host, and port all match. Intermediate redirect bodies are drained and hidden; only the final response reaches the caller.

The default limit is five redirects. Exceeding it, or an unresolvable `location`, ends the operation with `protocolError` and does not resend the original request.

## 3. Failures that retry repeats

Retry repeats only failures that currently make the target unavailable—such as network, DNS, or proxy CONNECT failures—and per-attempt timeouts. HTTP 4xx and 5xx status codes are not retry candidates. In `retry(attempts)`, attempts means additional attempts, so the total is `1 + attempts`.

Between attempts, the client uses exponential backoff with full jitter: the upper bound is `min(1 second, 50 ms × 2^attempt)`, and the delay is random from zero through that bound. Timeout applies to every attempt, so the maximum operation time grows with attempts and delays.

**Streaming uploads and downloads are not retried.** Their provider or sink cannot recreate bytes already read or delivered. Whether to start a new operation is an application decision based on idempotency and the server's duplicate-handling rule.

## 4. What the cookie jar stores

A cookie jar is client-local storage that saves response `Set-Cookie` values and attaches them to later requests. Without it, the client neither saves nor sends cookies. An enabled jar selects only exact host matches and ignores the `domain` attribute.

`path` defaults to `/` and matches only a path-segment prefix. A `secure` cookie is sent only over HTTPS, and `Max-Age` at or below zero deletes it immediately. `Expires`, `HttpOnly`, and `SameSite` are ignored. Each host keeps at most 128 cookies; the oldest is removed first. Malformed `Set-Cookie` values are ignored.

## 5. Common problems

| Symptom | Cause |
|---|---|
| Authentication disappears after a redirect. | The destination has a different origin, so `Authorization` was removed. |
| A POST body is absent after a redirect. | A POST on 301/302, or any request on 303, becomes GET and loses its body. |
| The server receives a request more than expected. | Retry repeated a transport failure or timeout. The application and server own duplicate handling. |
| A cookie is not sent to another subdomain. | The jar requires an exact host match and ignores `domain`. |

## 6. Next

- Status and JSON-decoding paths — [Response Rules](10-response-rules.en.md)
- Error kinds remaining after retry — [Error Handling](11-error-handling.en.md)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
