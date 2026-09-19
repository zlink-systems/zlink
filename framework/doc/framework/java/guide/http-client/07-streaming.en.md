---
title: "Streaming · Java"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/http-client/07-streaming.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Streaming

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Authentication, TLS, and Proxy](06-auth-tls-proxy.en.md) | [Next: Client and Request Lifecycle](08-client-lifecycle.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/http-client/07-streaming.en.md) · [C#/.NET](../../../dotnet/guide/http-client/07-streaming.en.md) · **Java** · [Kotlin](../../../kotlin/guide/http-client/07-streaming.en.md) · [Node/TypeScript](../../../node/guide/http-client/07-streaming.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can process response chunks through a download sink and send a request body through a body stream provider. The code in this chapter comes from each language's `HttpClient` tutorial.

Streaming moves a large body through a caller callback or provider. Unlike a JSON body, it does not serialize or accumulate the whole body. [Response Rules](10-response-rules.en.md) covers status and compression rules.

<!-- diagram: http-client-streaming -->
```mermaid
flowchart LR
    P[Body stream provider] --> U[HTTP upload]
    D[HTTP download] --> S[Download sink]
    U --> X[Target HTTP API]
    X --> D
```

The provider and sink are each consumed once while the body moves. This is why the streaming path does not apply replay or transparent decompression.

## 1. Receive a Response Through a Download Sink

A download sink is a callback invoked for each received body chunk. The final status and headers are returned as a raw response; bodies from intermediate redirect responses do not reach the sink.

```java
--8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-download-stream"
```

## 2. Send a Request Through a Body Stream Provider

A body stream provider returns the next chunk to send and signals completion with the language's empty value. Specify the content type with the provider.

```java
--8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-upload-stream"
```

## 3. Do Not Expect Retry or Decompression

A download sink and a body stream provider consume their body once and cannot recreate it. A streaming request is therefore excluded from retry and redirect replay. A download sink receives compressed response bytes unchanged; decompression is not applied.

## 4. Next Chapter

[Client and Request Lifecycle](08-client-lifecycle.en.md) covers client reuse, close timing, terminators, and timeout boundaries.
