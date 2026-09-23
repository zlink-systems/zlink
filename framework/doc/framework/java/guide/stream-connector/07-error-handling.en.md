---
title: "Error Handling · Java"
---

<!-- generated:start -->
<!-- This file is generated from `common/guide/stream-connector/07-error-handling.en.md`. Do not edit directly.
     Edit the common source instead, then regenerate with `python3 doc/site/scripts/generate_language_guides.py`. -->
<!-- generated:end -->

# Error Handling

<!-- framework-adapter-nav:start -->
[Contents](README.en.md) | [Previous: Connection Lifecycle](06-lifecycle.en.md) | [Next: Game Engine Integration](12-engine-integration.en.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
View in another language — [C++](../../../cpp/guide/stream-connector/07-error-handling.en.md) · [C#/.NET](../../../dotnet/guide/stream-connector/07-error-handling.en.md) · **Java** · [Kotlin](../../../kotlin/guide/stream-connector/07-error-handling.en.md) · [Node/TypeScript](../../../node/guide/stream-connector/07-error-handling.en.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "After reading this chapter"

    You can read the error code of a failed call and judge what that code means for the connection,
    so you can choose between retrying, recovering, and stopping.

The connector's error codes form a **closed set**. An implementation neither adds nor removes a
value, so once a handling is written per code, no new code appears to leave a branch missing. This
chapter covers how those codes reach the caller and what each one means.

## 1. Where Errors Arrive

The delivery differs by surface while the meaning stays the same. A surface that awaits completion
reports the failure there, a surface that takes a callback reports it through a result object, and
an error that belongs to no request is delivered as an error event. **In every case the receiver can
read the code.**

```java
try {
    LoginReply reply = connector.request(new LoginRequest("player-1", "tok-abc123"))
        .submit(LoginReply.class)
        .toCompletableFuture()
        .join();
} catch (CompletionException failure) {
    if (failure.getCause() instanceof ZLinkStreamException streamFailure
        && streamFailure.errorCode() == ZLinkStreamErrorCode.REQUEST_TIMEOUT) {
        retryLogin();
    }
}
```

**A standard exception of the language is never thrown as it is.** The standard types for a bad
argument or a bad state have no place to carry a code, so the caller cannot tell a configuration
error from a validation failure. A language that delivers errors as exceptions uses a dedicated
exception type that carries the code, and a build with exceptions off delivers the same code as a
result value.

## 2. Errors That Belong to No Request

A frame that could not be decoded, or a server error unrelated to any request, has no call waiting
for it, so it is delivered as an error event. This handler also returns a value that can be
released.

```java
AutoCloseable errors = connector.onErrorReceived(error -> {
    logError(error.code(), error.message());
    return CompletableFuture.completedFuture(null);
});
```

## 3. Error Codes

| Code | Meaning |
|---|---|
| `disconnected` | There is no connection, or it dropped |
| `configurationError` | The configuration is wrong — endpoint scheme against transport, a transport the runtime does not support |
| `validationFailed` | A pre-send check, an option range check, or an observation condition of a wait surface did not hold |
| `requestTimeout` | The wait for an answer ran out of time |
| `connectTimeout` | The wait for a connection ran out of time |
| `FrameDecodeFailed` | A frame or header could not be decoded |
| `FrameTooLarge` | A received payload exceeded the receive limit |
| `SendFailed` | The transmission failed |
| `CompressionFailed` | Compression failed |
| `DecompressionFailed` | Decompression failed |
| `TlsValidationFailed` | TLS validation failed |
| `userCallbackFailed` | A user callback failed |
| `RemoteError` | The server answered with an error |

**The two codes for running out of time mean different things.** A request that ran out of time did
not receive its answer; a wait surface that ran out of time observed something other than what was
expected. That is why the second is reported as a validation failure — the caller has to handle the
two cases differently.

To return a domain error as a normal answer, the server uses a successful answer with its own
payload rather than an error answer. The payload of an error answer is always JSON carrying a code
and a message, whatever the codec setting is.

## 4. What an Error Means for the Connection

A failure that ends only the current call and a failure that ends the connection call for different
handling.

| Code | Current call | Connection | Automatic reconnect |
|---|---|---|---|
| `configurationError` · `validationFailed` | fails | kept | no |
| `requestTimeout` | only that request fails | kept | no |
| `connectTimeout` · `TlsValidationFailed` | connect fails | disconnected | applies the attempt policy |
| `disconnected` · `SendFailed` | the call in progress fails | disconnected if the transport dropped | applies it when enabled |
| `FrameDecodeFailed` (frame or header) · `FrameTooLarge` | the frame is not delivered and pending requests fail | ends | applies it when enabled |
| `CompressionFailed` | only that send fails | kept | no |
| `DecompressionFailed` | only that received packet or pending request fails | kept | no |
| `userCallbackFailed` · `RemoteError` | delivered as an error event or to the related call | kept | no |

Where the connection ends, the close reason is recorded as a transport error. Reading the close
reason is covered by [Connection Lifecycle](06-lifecycle.en.md).

## 5. Common Handling

**No connection.** A send that fails as no connection means reconnection is in progress or has
already been given up. Register a connection-state handler and send again once the connection is
back. Packets whose value expires with time are not sent again.

**Answer timed out.** Only that request failed and the connection stayed, so the same request can be
sent again. The server may have processed it already and only the answer was late, so a request that
must not be processed twice is made recognizable on the server side.

**Receive limit exceeded.** A received payload over the receive limit is not delivered and the
connection ends. If the server is configured to send larger packets, raise the receive limit in
[Connector Options](03-connector-options.en.md).

**Server error answer.** An error answer from the server fails that request, and one that matches no
request is delivered as an error event. The connection is kept, so other packets are unaffected.

## 6. A Code That Does Not Occur on Every Runtime

TLS validation failure does not occur on a browser runtime, because the browser's WebSocket API does
not distinguish a TLS failure from an ordinary connection failure. The code stays in the set and is
simply unused there, so a branch written per code does not change from runtime to runtime.

## 7. Related Chapters

- Finding out why a connection ended — [Connection Lifecycle](06-lifecycle.en.md)
- Limits and when they are validated — [Connector Options](03-connector-options.en.md)
- Where a send fails — [Sending Packets](04-sending.en.md)
