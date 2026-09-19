---
title: "Streaming · Kotlin"
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
View in another language — [C++](../../../cpp/guide/http-client/07-streaming.en.md) · [C#/.NET](../../../dotnet/guide/http-client/07-streaming.en.md) · [Java](../../../java/guide/http-client/07-streaming.en.md) · **Kotlin** · [Node/TypeScript](../../../node/guide/http-client/07-streaming.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    This chapter processes a response in chunks and sends a request body in parts. Its code comes from each language's `HttpClient` tutorial.

Streaming moves a large body through a caller callback or provider. Unlike a JSON body, it does not serialize or accumulate the whole body. [Response Rules](10-response-rules.en.md) covers status and compression rules.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-streaming-en.html"
        title="http client streaming" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-streaming-en.html" target="_blank">↗ 크게 보기</a></p>

The provider and sink are each consumed once while the body moves. This is why the streaming path does not apply replay or transparent decompression.

## 1. Response Chunks Flow Through a Download Sink

A download sink is a callback invoked for each received body chunk. A raw response carries status, headers, and the body before JSON decoding. The final raw response from a download preserves status and headers; bodies from intermediate redirect responses do not reach the sink.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-download-stream"
```

The example counts each export-body chunk and accumulates the total number of chunks and bytes.

## 2. Request Chunks Come from a Body Stream Provider

A body stream provider returns the next chunk to send and signals completion with the language's empty value. Its configuration includes the content type.

```kotlin
--8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-upload-stream"
```

The example supplies three NDJSON lines in sequence and confirms that the server imported three items.

## 3. Retry and Decompression Are Excluded

A download sink and a body stream provider consume their body once and cannot recreate it. A streaming request is therefore excluded from retry and redirect replay. A download sink receives compressed response bytes unchanged; decompression is not applied.

## 4. Next Chapter

[Client and Request Lifecycle](08-client-lifecycle.en.md) covers client reuse, close timing, response completion, and timeout boundaries.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
