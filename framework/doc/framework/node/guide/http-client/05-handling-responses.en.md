---
title: "Handling Responses · Node/TypeScript"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/http-client/05-handling-responses.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Handling Responses

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Request Bodies](04-request-body.en.md) | [Next: Authentication, TLS, and Proxy](06-auth-tls-proxy.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/http-client/05-handling-responses.en.md) · [C#/.NET](../../../dotnet/guide/http-client/05-handling-responses.en.md) · [Java](../../../java/guide/http-client/05-handling-responses.en.md) · [Kotlin](../../../kotlin/guide/http-client/05-handling-responses.en.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    This chapter distinguishes JSON-decoded bodies from original HTTP response data. Its code comes from each language's `HttpClient` tutorial.

The HTTP client changes only the response form on the same request builder. A typed response carries status, headers, and a JSON-decoded body. A raw response carries status, headers, and the body before JSON decoding. [Response Rules](10-response-rules.en.md) covers status-specific handling.

## 1. A Typed Response Is the Default

A typed response is the default for a successful JSON API call. It ends as an exception when the HTTP status is 400 or higher, while a raw response preserves an error body. A terminator is the final call that submits the gathered request and selects its response form.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-request-response-en.html"
        title="http client request response" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-request-response-en.html" target="_blank">↗ 크게 보기</a></p>

The response form does not change the request sent. It determines whether application code keeps status and headers or leaves only a JSON body value.

```typescript
--8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-response-kinds"
```

The example reads the same player response as typed, raw, and body-only values, exposing its status, content type, and nickname respectively.

## 2. A Raw Response Preserves HTTP Details

A 4xx or 5xx response is returned as a raw response when its transfer succeeds. This form supports branches based on status and access to an API error body.

The body-only terminator applies when only the body is required. It leaves only the successful JSON value in the calling flow and does not return status or headers.

!!! note "Choose the response form first"

    A typed response fits a normal JSON API, a raw response fits direct HTTP status and error-body handling, and a body-only response fits a path that needs no other response information.

## 3. Compression Is Opt-in

A client with `compression` fits an API that sends compressed responses while the application consumes ordinary JSON responses. [Response Rules](10-response-rules.en.md) covers the detailed behavior.

```typescript
--8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-compressed-response"
```

The example confirms that the compression-enabled client returns the room response as a normal status-200 response.

## 4. Next Chapter

[Authentication, TLS, and Proxy](06-auth-tls-proxy.en.md) covers credentials, certificates, and proxy configuration.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
