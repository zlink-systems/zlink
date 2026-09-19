# Request Bodies

!!! info "After reading this chapter"

    You can send JSON as the usual request body and select the appropriate body form when an API
    requires form or multipart data.

A request body is attached to a request that sends content, such as POST, PUT, or PATCH. The HTTP client uses a typed JSON body as the default path for serializing an application DTO.

## 1. Send a JSON Body

A typed JSON body sends an application DTO as `application/json`. The response form is independent of the body format, so choose a typed response, raw response, or a body-only response as needed.

<!-- diagram: http-client-request-body -->
```mermaid
flowchart LR
    Dto[Application DTO] --> Json[JSON body] --> Request
```

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-json-body"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-json-body"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-json-body"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-json-body"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-json-body"
    ```

## 2. Choose Form or Multipart

A form suits an API that accepts short name and value pairs as `application/x-www-form-urlencoded`. Multipart is `multipart/form-data` for sending text fields and file fields in one request. Form fields accumulate, and multipart fields and files accumulate into one body.

!!! info "Choose one body source"

    Mixing typed JSON, raw content, a stream, a form, or multipart in one request ends with
    `ProtocolError` before the request is sent.

## 3. Boundary for Raw and Streaming Bodies

A raw body with an arbitrary content type and a streaming upload that supplies byte chunks are separate paths from JSON, form, and multipart. [Response Rules](10-response-rules.en.md) covers raw-body response handling, and [Streaming](07-streaming.en.md) covers streaming uploads.

## Next Chapter

[Handling Responses](05-handling-responses.en.md) chooses a JSON-decoded response, a raw response, or a body-only response.
