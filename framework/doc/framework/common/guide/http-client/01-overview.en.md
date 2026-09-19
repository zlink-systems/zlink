# HTTP Client Overview

!!! info "After reading this chapter"

    You can decide when an application inside or outside the server framework should use the HTTP client
    for an external HTTP API, and know its boundary.

The HTTP client is a client-side library that sends application requests to an external HTTP API and receives responses. The five languages differ only in names and notation; they provide the same semantics for building requests and choosing responses.

## 1. Where to Use the HTTP Client

A handler in the server framework, a CLI, a batch process, and a separate client process can all call an external HTTP API. A repeated-call application creates one client and reuses it. A single call can use a one-shot, which builds a request directly from the builder.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-overview-en.html"
        title="http client overview" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-overview-en.html" target="_blank">↗ 크게 보기</a></p>

The following example previews the first request with a typed response. A typed response contains the status, headers, and a JSON-decoded body.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-first-request"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-first-request"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-first-request"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-first-request"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-first-request"
    ```

All five examples use the same builder semantics to send a profile GET request, decode its JSON body as a typed `PlayerProfile` response, and read the player ID and nickname.

## 2. Boundary with the Server HTTP Surface

The HTTP client calls an external API. It does not open server HTTP routes or receive server requests, and it is not a browser replacement for `fetch`.

The HTTP client provides request methods, headers, bodies, and response rules. The external API's routes, authentication policy, and request and response DTOs belong to the application.

## 3. Tutorial Flow

The `HttpClient` tutorial for each language runs client creation, JSON requests, response forms, authentication, streams, and errors in one process. Its first step is [Installation and the First Request](02-getting-started.en.md).

## Next Chapter

[Installation and the First Request](02-getting-started.en.md) adds the package and runs the first GET request.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
