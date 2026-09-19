# Installation and the First Request

!!! info "After reading this chapter"

    You can add the HTTP client package to a project, receive a typed GET response through one client,
    and choose a one-shot for a single request.

The HTTP client ships separately from the server framework. A calling process needs only the HTTP client package. The tutorial below creates a client and reads one profile.

## 1. Package Reference

=== "C#/.NET"

    ```bash
    dotnet add package Zlink.HttpClient
    ```

=== "C++"

    ```cmake
    find_package(zlink_http_client_cpp CONFIG REQUIRED)
    target_link_libraries(my_client PRIVATE zlink::http_client)
    ```

=== "Java"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/java/HttpClient/build.gradle.kts:http-client-dependency"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/build.gradle.kts:http-client-dependency"
    ```

=== "Node/TypeScript"

    ```bash
    npm install @zlink-systems/http-client
    ```

Each tab shows where that language references the package. The Java and Kotlin tabs quote the tutorial's actual Gradle dependencies, while Node/TypeScript installs the public npm package.

## 2. Client Creation and the First GET Request

A builder holding the base URL and default options creates a client. A request builder is created when a method such as GET or POST is selected from the client. A typed response terminator decodes the JSON body to the requested type and returns the response envelope. An asynchronous terminator does not occupy a thread or event loop while it waits for the result.

<iframe class="zlink-diagram" src="/common/diagrams/http-client-first-request-en.html"
        title="http client first request" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/http-client-first-request-en.html" target="_blank">↗ 크게 보기</a></p>

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-client-create"
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-first-request"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-client-create"
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-first-request"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-client-create"
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-first-request"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-client-create"
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-first-request"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-client-create"
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-first-request"
    ```

The code creates a client with a three-second default timeout, sends a profile GET request, and prints the player ID and nickname from the typed response body.

## 3. Result

The output below comes from the tutorial (`framework/languages/<language>/tutorial`) HttpClient with its Server and Client started as described in the README's "Run" section; the address is the Client's HTTP surface.

=== "C#/.NET"

    ```text
    first request: p1 rookie
    ```

=== "C++"

    ```text
    # (#714 will fill this after its fix)
    ```

=== "Java"

    ```text
    first request: p1 rookie
    ```

=== "Kotlin"

    ```text
    first request: p1 rookie
    ```

=== "Node/TypeScript"

    ```text
    first request: p1 rookie
    ```

The captured runs for four languages show the same player ID and nickname. The C++ result remains reserved for the #714 fix.

## 4. A Single Request

A one-shot obtains its request builder directly from the client builder. It closes the client after completion, so it does not reuse a connection pool. A service that calls the same API repeatedly keeps a client as in the preceding section.

## Next Chapter

[Making Requests](03-making-requests.en.md) sets methods, paths, queries, headers, and per-request timeouts. [Handling Responses](05-handling-responses.en.md) chooses a response form, and [Client and Request Lifecycle](08-client-lifecycle.en.md) covers reuse and closing.

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d&&d.body?d.body.scrollHeight:0;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
