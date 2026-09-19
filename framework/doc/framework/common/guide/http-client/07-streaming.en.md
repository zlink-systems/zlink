# Streaming

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

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-download-stream"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-download-stream"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-download-stream"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-download-stream"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-download-stream"
    ```

## 2. Send a Request Through a Body Stream Provider

A body stream provider returns the next chunk to send and signals completion with the language's empty value. Specify the content type with the provider.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-upload-stream"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-upload-stream"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-upload-stream"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-upload-stream"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-upload-stream"
    ```

## 3. Do Not Expect Retry or Decompression

A download sink and a body stream provider consume their body once and cannot recreate it. A streaming request is therefore excluded from retry and redirect replay. A download sink receives compressed response bytes unchanged; decompression is not applied.

## 4. Next Chapter

[Client and Request Lifecycle](08-client-lifecycle.en.md) covers client reuse, close timing, terminators, and timeout boundaries.
