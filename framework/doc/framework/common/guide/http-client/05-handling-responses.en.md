# Handling Responses

!!! info "After reading this chapter"

    You can choose typed, raw, or body-only responses for their intended use. The code in this chapter comes from each language's `HttpClient` tutorial.

The HTTP client changes only the response form on the same request builder. A typed response is the default for reading a JSON body as a type; choose a raw response when application code must inspect status and headers. [Response Rules](10-response-rules.en.md) covers status-specific handling.

## 1. Use a Typed Response by Default

A typed response carries the status, headers, and a JSON-decoded body. Use it for a successful JSON API call. A typed response ends as an exception when the HTTP status is 400 or higher, so choose a raw response when the error body is required.

<!-- diagram: http-client-request-response -->
```mermaid
flowchart LR
    R[Request builder] --> T[Terminator]
    T --> H[HTTP response]
    H --> Y[typed response\nstatus, headers, body]
    H --> W[raw response\nstatus, headers, raw body]
    Y --> B[body-only value]
```

The response form does not change the request sent. It determines whether application code keeps status and headers or leaves only a JSON body value.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-response-kinds"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-response-kinds"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-response-kinds"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-response-kinds"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-response-kinds"
    ```

## 2. Read HTTP Details from a Raw Response

A raw response carries the status, headers, and body before JSON decoding. A 4xx or 5xx response is returned as a raw response when its transfer succeeds, so use it when branching on a status or reading an API error body.

Use the body-only terminator when only the body is required. It leaves only the successful JSON value in the calling flow and does not return status or headers.

!!! note "Choose the response form first"

    Use a typed response for a normal JSON API, a raw response where HTTP status and error body are handled directly, and a body-only response where no other response information is needed.

## 3. Read a Compressed Response Transparently

`Compression` requests `gzip` and `deflate`, then decodes the response body. It also removes the `content-encoding` header from the decoded response, so application code handles the body as if no compression were used. The body limit still applies after decoding.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-compressed-response"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-compressed-response"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-compressed-response"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-compressed-response"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-compressed-response"
    ```

## 4. Next Chapter

[Authentication, TLS, and Proxy](06-auth-tls-proxy.en.md) covers credentials, certificates, and proxy configuration.
