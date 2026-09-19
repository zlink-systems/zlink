# Making Requests

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

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-request-shaping"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-request-shaping"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-request-shaping"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-request-shaping"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-request-shaping"
    ```

## 3. Values That Change Per Request

Put request-specific credentials, trace IDs, queries, and timeouts on the request builder. Configure headers and credentials that apply to every request when the client is created. A timeout is the time boundary for one request; retry and response-status rules are covered in [Redirect, Retry, and Cookies](09-redirect-retry-cookie.en.md) and [Response Rules](10-response-rules.en.md).

## Next Chapter

[Request Bodies](04-request-body.en.md) adds JSON, form, and multipart bodies.
