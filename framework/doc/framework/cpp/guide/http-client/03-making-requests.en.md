---
title: "Making Requests · C++"
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
View in another language — **C++** · [C#/.NET](../../../dotnet/guide/http-client/03-making-requests.en.md) · [Java](../../../java/guide/http-client/03-making-requests.en.md) · [Kotlin](../../../kotlin/guide/http-client/03-making-requests.en.md) · [Node/TypeScript](../../../node/guide/http-client/03-making-requests.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can choose an HTTP method and path, then shape one request for an external API with
    queries, headers, and a per-request timeout.

A request builder is created when a method is selected from the client. It accumulates the request shape until a terminator asks for a response. A path starts with `/` and follows the base URL.

## 1. Choose a Method and Path

The HTTP client provides the seven methods GET, POST, PUT, DELETE, PATCH, HEAD, and OPTIONS. GET reads a resource; POST starts work or creates one. C++ spells DELETE as `delete_` because of its keyword.

A path that does not start with `/` ends with `ProtocolError` before a request is sent. Keep the base URL and path separate in one request. Use another client or a one-shot when another base URL is needed.

## 2. Add Queries, Headers, and a Timeout

A query accumulates names and values after URL encoding. A header applies only to that request and overrides a client default header with the same name. A per-request timeout replaces the client's default timeout only for that request.

<!-- diagram: http-client-request-shaping -->
```mermaid
flowchart LR
    BaseUrl[Base URL] --> Path --> Query
    Query --> Header --> Timeout --> Request
```

```cpp
--8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-request-shaping"
```

## 3. Values That Change Per Request

Put request-specific credentials, trace IDs, queries, and timeouts on the request builder. Configure headers and credentials that apply to every request when the client is created. A timeout is the time boundary for one request; retry and response-status rules are covered in [Redirect, Retry, and Cookies](09-redirect-retry-cookie.en.md) and [Response Rules](10-response-rules.en.md).

## Next Chapter

[Request Bodies](04-request-body.en.md) adds JSON, form, and multipart bodies.
