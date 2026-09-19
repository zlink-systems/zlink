# Error Handling

!!! info "After reading this chapter"

    You can read a failure kind from an HTTP client operation and decide whether to start a new operation after automatic retry ends.

An error kind is a closed classification for deciding how to recover from a failed request. It describes the actual failure in request construction, transport, timeout, or response validation—not an HTTP status alone. Caller cancellation is not converted to this classification; it remains each language's cancellation result.

## 1. Kinds and failure situations

| Situation | Kind |
|---|---|
| The builder shape, body-source combination, typed JSON decode, decompression, or redirect shape is invalid. | `ProtocolError` |
| The network, DNS, proxy CONNECT, or target connection is currently unavailable. | `Unavailable` |
| The configured response-body byte limit was exceeded. | `Rejected` |
| A per-attempt timeout elapsed. | `DeadlineExceeded` |
| A typed response has HTTP status 400 or higher, or an execution failure has no other kind. | `InternalFailure` |

Enum spelling is `protocol_error`, `unavailable`, `rejected`, `deadline_exceeded`, and `internal_failure` in C++; PascalCase in .NET and Node/TypeScript; and `PROTOCOL_ERROR`, `UNAVAILABLE`, `REJECTED`, `DEADLINE_EXCEEDED`, and `INTERNAL_FAILURE` in Java and Kotlin.

## 2. Receiving a failure and reading its kind

The tutorial catches both an error status from a typed request and a connection failure to a closed port, then prints their kinds. A terminator is the final request-builder call that starts transmission and waits for a response; its shape varies by language. The decision after catching a failure is the same.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/tutorial/HttpClient/Program.cs:http-error-kinds"
    ```

    ```console
    error kinds: bad request InternalFailure connection refused Unavailable
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/tutorial/HttpClient/main.cpp:http-error-kinds"
    ```

    ```console
    # (#714 수정 뒤 채운다)
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/tutorial/java/HttpClient/src/main/java/systems/zlink/tutorial/httpclient/HttpClientProgram.java:http-error-kinds"
    ```

    ```console
    error kinds: bad request INTERNAL_FAILURE connection refused INTERNAL_FAILURE
    # From 0.19.0, connection refused is UNAVAILABLE.
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/tutorial/kotlin/HttpClient/src/main/kotlin/systems/zlink/tutorial/httpclient/HttpClientProgram.kt:http-error-kinds"
    ```

    ```console
    error kinds: bad request INTERNAL_FAILURE connection refused INTERNAL_FAILURE
    # From 0.19.0, connection refused is UNAVAILABLE.
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/tutorial/HttpClient/main.ts:http-error-kinds"
    ```

    ```console
    error kinds: bad request InternalFailure connection refused Unavailable
    ```

## 3. What to decide after retry

Automatic retry repeats transport failures that produce `Unavailable` and `DeadlineExceeded` only within the configured operation. When all attempts end, the caller receives the final kind. `ProtocolError`, `Rejected`, HTTP 4xx/5xx status, and streaming requests are not retry candidates.

Exceptions and results do not carry a retry hint. An application must not automatically start the same operation based on the kind alone; it must also decide whether duplicate execution is safe and whether the server may already have processed the request.

## 4. Common problems

| Symptom | Cause |
|---|---|
| A 400 response is handled as `ProtocolError`. | Status 400 or higher on a typed path is `InternalFailure`. Use raw when an error payload is needed. |
| A large JSON response appears to be a network failure. | Exceeding the response-body limit is `Rejected`, including an expanded compressed body. |
| The server receives a request again after a timeout. | Configured retry repeated `DeadlineExceeded`. |
| Cancellation is handled as a Framework kind. | Caller cancellation remains the language's cancellation result. |

## 5. Next

- Reading a 4xx/5xx payload with a raw response — [Response Rules](10-response-rules.en.md)
- Conditions that exclude retry and streaming — [Redirect, Retry, and Cookies](09-redirect-retry-cookie.en.md)
