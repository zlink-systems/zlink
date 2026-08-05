# 07 — Error Handling

[← Connection Lifecycle](06-lifecycle.en.md) | [Table Of Contents](INDEX.en.md) | [Next: E2E Client →](08-e2e-client.en.md)

---

## result_t\<T\>

Every synchronous API returns a `result_t<T>`. No exception is thrown.

```cpp
auto reply = connector.request(request).submit<match_join_reply_t>();

if (!reply) {
    // failure
    auto code = reply.error_code();
    auto msg  = reply.error() ? reply.error()->message : "";
    return;
}

auto value = reply.value(); // T&&
```

| Expression | Meaning |
|--------|------|
| `if (result)` | check success |
| `result.value()` | the success value (UB if called on a failed state) |
| `result.error_code()` | the `error_code_t` enum value |
| `result.error()` | `const error_t*`. Includes the message. nullptr on success |

## The error_code_t List

| Code | Meaning | Main API Where It Occurs |
|------|------|--------------|
| `disconnected` | Operation called with no connection, or a transport drop | send, request, wait, dispatch |
| `configuration_error` | Invalid settings like endpoint, packet name, timeout | connect, send, request |
| `validation_failed` | The request argument is outside the contract range | send, request |
| `request_timeout` / `wait_timeout` | The reply or wait-target packet didn't arrive within the timeout | request, wait_for |
| `connect_timeout` | The connect attempt didn't complete within `connect_timeout` | connect |
| `frame_decode_failed` | The received frame can't be parsed to the STREAM contract | receive loop |
| `frame_too_large` | The send payload or metadata exceeds the configured limit | send, request |
| `send_failed` | The connection is open, but the packet write failed | send, request |
| `unsupported_codec` | Using a codec not in the build | send, request |
| `compression_failed` | The payload can't be compressed with the configured compression codec | send, request |
| `tls_validation_failed` | TLS server certificate verification failed | connect (TLS/WSS) |
| `decompression_failed` | The compressed payload can't be restored with the configured compression codec | receive loop |
| `user_callback_failed` | An exception occurred inside the `on<T>()` callback | dispatch |
| `remote_error` | The server responded with an error frame | request |
| `closed` | A pending operation ended because `close()` was called | every pending operation |
| `canceled` | The operation ended due to coroutine task destruction or explicit cancellation | e2e client awaiter |

## Handling By Pattern

### Retry On Timeout

```cpp
auto reply = connector
    .request(query)
    .timeout(std::chrono::seconds{5})
    .submit<match_data_t>();

if (!reply && reply.error_code() == zsc::error_code_t::request_timeout) {
    // retry or fall back
}
```

### Handling disconnected

If `send()` or `request()` returns `disconnected`, a reconnect is in progress or has already failed.
Subscribe to status events and retry after the reconnect completes.

```cpp
connector.on_connection_state_changed([&](const zsc::connection_state_changed_t& ev) {
    if (ev.state == zsc::connection_state_t::connected) {
        // retry pending work after the reconnect succeeds
    }
});
```

### remote_error

Occurs when the server returns an error frame.

```cpp
if (!reply && reply.error_code() == zsc::error_code_t::remote_error) {
    auto msg = reply.error() ? reply.error()->message : "unknown";
    // msg: the error message the server sent
}
```

### Preventing frame_too_large

```cpp
// use compress() when the payload could be large
connector
    .send(large_payload_t{data})
    .compress()
    .submit();

// or raise the limit in the options
options.max_send_payload_size = 512 * 1024;
options.max_receive_payload_size = 512 * 1024;
```

If compression is explicitly off and `.compress()` is called, `compression_failed` occurs. In the
same state, receiving a compressed frame results in `decompression_failed`, and the error message
reveals that a compression codec wasn't configured. Even a decompressed payload that again exceeds
`max_receive_payload_size` is treated as `frame_too_large`.

## The Throwing Adapter

If you need a helper that throws an exception instead of `result_t<T>`, use
`zlink/stream_connector_throwing.hpp`.

```cpp
#include <zlink/stream_connector_throwing.hpp>

// Returns TReply on success, or throws zlink::stream_connector::stream_error on failure
auto reply = zlink::stream_connector_throwing::request<match_join_reply_t>(
    connector, request);
```

The throwing adapter is an optional surface for server framework or tool code. A game engine client
uses the core `result_t<T>` API directly.

## A Success-Only Happy Path Pattern

```cpp
auto connected = connector.connect();
if (!connected) { return; }

auto auth = connector
    .request(auth_request_t{"player-1", "tok-abc123"})
    .submit<auth_reply_t>();
if (!auth) { return; }

connector
    .send(enter_world_t{auth.value().session_id, "zone-12"})
    .submit();
```
