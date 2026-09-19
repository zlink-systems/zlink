---
title: "Making Requests · Java"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/http-client/03-making-requests.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Making Requests

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Installation and the First Request](02-getting-started.en.md) | [Next: Request Bodies](04-request-body.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/http-client/03-making-requests.en.md) · [C#/.NET](../../../dotnet/guide/http-client/03-making-requests.en.md) · **Java** · [Kotlin](../../../kotlin/guide/http-client/03-making-requests.en.md) · [Node/TypeScript](../../../node/guide/http-client/03-making-requests.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can choose an HTTP method and path, then shape one request for an external API with
    queries, headers, and a per-request timeout.

A request builder is created when a method is selected from the client. It accumulates the request shape until a terminator asks for a response. A terminator is the final call that finishes the builder, submits the request, and selects the response form. A path starts with `/` and follows the base URL.

## 1. Methods and Paths

The HTTP client provides the seven methods GET, POST, PUT, DELETE, PATCH, HEAD, and OPTIONS. GET reads a resource; POST starts work or creates one. C++ spells DELETE as `delete_` because of its keyword.

A path that does not start with `/` ends with `protocolError` before a request is sent. One request keeps its base URL and path separate. Another base URL uses a separate client or a one-shot.

## 2. Queries, Headers, and a Timeout

A query accumulates names and values after URL encoding. A header applies only to that request and overrides a client default header with the same name. A per-request timeout replaces the client's default timeout only for that request.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-request-shaping-en.html"
        title="http client request shaping" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-request-shaping-en.html" target="_blank">↗ 크게 보기</a></p>

```java
--8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-request-shaping"
```

The code uses separate base URLs for the regular and administrative APIs, adds a per-request header, timeout, and query, then reads the status and changed weight from typed responses.

## 3. Values That Change Per Request

Request-specific credentials, trace IDs, queries, and timeouts belong on the request builder. Headers and credentials that apply to every request belong on the client. A timeout is the time boundary for one attempt, and retry applies it to each attempt. Retry and response-status rules are covered in [Redirect, Retry, and Cookies](09-redirect-retry-cookie.en.md) and [Response Rules](10-response-rules.en.md).

## Next Chapter

[Request Bodies](04-request-body.en.md) adds JSON, form, and multipart bodies.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
