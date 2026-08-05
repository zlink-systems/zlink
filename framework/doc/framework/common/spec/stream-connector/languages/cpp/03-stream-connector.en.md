<!-- framework-adapter-nav:start -->
[Document list](../../../../../../README.en.md)
<!-- framework-adapter-nav:end -->

# C++ Stream Connector Public Contract

> This document is the **C++ projection** of the
> [Stream Connector Common Spec](../../32-stream-connector.en.md).
> The execution environment, transport, wire, packet model, lifecycle,
> error meaning, and default value are owned by the common spec.
> This document only fixes the exact public interface expressing that
> meaning in C++.

## 1. Package And Entrypoint

A plain C++ client uses the CMake target `zlink::stream_connector`.
The public type is in the `zlink::stream_connector` namespace, and the
whole connector contract is imported through the header below.

```cpp
#include <zlink/stream_connector/contracts/connector.hpp>
```

A connector is built by the factory as a value object. It copies
options at creation and doesn't expose the implementation detail type.

```cpp
static connector_t connector_factory_t::create(connector_options_t options);
```

## 2. `connector_t`

`connector_t` provides the entry point for connection state, lifecycle,
and packet operations.

```cpp
enum class stream_close_reason_t : std::uint8_t {
    client_close = 1,
    idle_timeout = 2,
    heartbeat_timeout = 3,
    server_drain = 4,
    protocol_error = 5,
    transport_error = 6
};

bool is_connected() const;
connection_state_t state() const;
std::optional<stream_close_reason_t> close_reason() const;
connector_options_t options() const;
std::size_t pending_dispatch_count() const;

result_t<void> connect();                                  // waits for the connection result in the current call.
void connect(std::function<void(result_t<void>)> callback); // receives the connection result as a callback.
result_t<void> close();                                    // waits for the close result in the current call.
void close(std::function<void(result_t<void>)> callback);   // receives the close result as a callback.
result_t<void> dispatch();                                 // runs one pending callback of Manual mode.
```

A push callback is registered with `on<T>(...)`. In
`dispatch_mode_t::manual`, `dispatch()` runs the callback, and in
`dispatch_mode_t::immediate`, the receive path runs the callback. The
`wait_for` family directly consumes a matching packet from the receive
queue in both modes.

`close_reason()` returns an empty value if it hasn't disconnected yet.
The close reason's closed value and meaning is owned by
[Common Spec §6.3](../../32-stream-connector.en.md#63-close-reason).
enum values 1-6 are the same as the `session-closing` wire value, but
the codec explicitly converts them and doesn't cast the enum to an
integer to build a frame.

## 3. Send And Wait Builder

A typed `send` and `request` decide the packet name from the message
type. A raw packet overload is also provided. Each call returns a
builder, and executes only once the terminator `submit` is called.

```cpp
send_call_t send(const TMessage& message);
request_call_t request(const TRequest& request);

send_call_t& packet_name(std::string name); // overrides the packet name when interworking with an external protocol.
send_call_t& metadata(std::string key, std::string value);
send_call_t& metadata(metadata_t metadata);
send_call_t& compress();
void submit(); // starts a one-way send on the connector core's existing no-coroutine boundary.

request_call_t& packet_name(std::string name);
request_call_t& metadata(std::string key, std::string value);
request_call_t& metadata(metadata_t metadata);
request_call_t& timeout(std::chrono::milliseconds timeout);
request_call_t& compress();
result_t<TReply> submit<TReply>(); // waits for a matching-correlation reply and decodes it as TReply.
void submit<TReply>(std::function<void(result_t<TReply>)> callback);
```

A single push wait is handled by `wait_call_t<TMessage>`.

```cpp
wait_call_t<TMessage> wait_for<TMessage>();
wait_call_t<TMessage> wait_for<TMessage>(std::string packet_name);

wait_call_t<TMessage>& where(std::function<bool(const TMessage&)> predicate);
wait_call_t<TMessage>& timeout(std::chrono::milliseconds timeout);
result_t<TMessage> submit(); // consumes and decodes one matching unread packet.
void submit(std::function<void(result_t<TMessage>)> callback);
```

The one-way `submit()` doesn't return a result. Since the C++ connector
core keeps the common contract's no-exception/no-coroutine boundary, a
new `task_t` isn't introduced for this terminal. A send failure is
reported through the existing connector error event. Request and wait
keep the existing result type and also provide a callback completion
path.

Typed `send`, `request`, `on`, and `wait_for` all use the single codec
put in `connector_options_t::typed_codec` together. If not specified,
the JSON codec is used. Protobuf, MessagePack, and a user codec
extension provide a `typed_codec_t` implementation, put into options
once when building a connector. A public API for registering a codec
per message type, or choosing a codec per send/request operation, isn't
provided. A Raw encoded payload uses the codec number already recorded
in the payload as is, for external protocol interworking.

## 4. Test Wait Interface

The behavioral contract is owned by
[Common Spec §10.2](../../32-stream-connector.en.md).

### 4.1 Push Observation — Connector Method

`expect_none` and `wait_for_sequence` are `connector_t` methods in the
same spot as `wait_for`. Both an overload that decides the packet name
from the type name and an overload where the caller specifies the
packet name are provided.

```cpp
expect_none_call_t<TMessage> expect_none<TMessage>();
expect_none_call_t<TMessage> expect_none<TMessage>(std::string packet_name);
expect_none_call_t<packet_t> expect_none(std::string packet_name);

wait_for_sequence_call_t<TMessage> wait_for_sequence<TMessage>();
wait_for_sequence_call_t<TMessage> wait_for_sequence<TMessage>(std::string packet_name);
wait_for_sequence_call_t<packet_t> wait_for_sequence(std::string packet_name);
```

A negative observation must specify a positive window. If a packet of
the same name arrives within the window, it fails with
`validation_failed`; if it doesn't arrive, it succeeds.

```cpp
auto result = connector.expect_none<order_changed_t>()
                .within(std::chrono::milliseconds(100)) // there must be no same push during this time.
                .submit();
```

A sequence observation applies each `expect` predicate to a push of the
same name in arrival order. It uses one overall timeout, and on
success returns the decoded payload list. This is a contract that
verifies arrival **in the specified order**, not simply whether N
arrived.

```cpp
auto result = connector.wait_for_sequence<order_changed_t>()
                .expect([](const auto& value) { return value.status == status_t::paid; })
                .expect([](const auto& value) { return value.status == status_t::shipped; })
                .timeout(std::chrono::seconds(2)) // the overall time limit satisfying both predicates.
                .submit();
```

Both builders provide the `result_t`-returning form of `submit()` and
the `submit(...)` form that takes a callback. A status-only method
isn't provided. Since status is a payload field, a single observation
is expressed as `wait_for<T>().where(...)`, and a sequence observation
as `wait_for_sequence<T>().expect(...)`. Domain REST polling is the
HTTP client's responsibility and isn't included in the connector
interface.

### 4.2 Test Assertion Helper

The common E2E and application test can use a helper from the
namespace below. This helper handles the repeated branching and
diagnostic generation in one place when checking a connector's error
result.

```cpp
namespace zlink::stream_connector::assertions
{
void ensure(bool condition, std::string_view message);

template <typename TAction>
error_t expect_failure(
  TAction&& action,
  std::optional<error_code_t> expected_kind = std::nullopt);

template <typename TAction>
error_t expect_timeout(TAction&& action);
}
```

`ensure` fails with the given diagnostic message if the condition is
false. An empty diagnostic message isn't allowed. `expect_failure`
returns the action's failure result, and if an error kind is
specified, also checks whether it matches. `expect_timeout` only
returns a request or connect timeout, and delivers a different failure
as is.

## 5. Result And Error

`error_code_t` is the following closed value set. Each value's meaning
and effect on the operation/connection corresponds one-to-one with the
[common error table](../../32-stream-connector.en.md#9-error-meaning).

```cpp
enum class error_code_t
{
    disconnected,
    configuration_error,
    validation_failed,
    request_timeout,
    connect_timeout,
    frame_decode_failed,
    frame_too_large,
    send_failed,
    compression_failed,
    tls_validation_failed,
    decompression_failed,
    user_callback_failed,
    observer_failed,
    observer_dropped,
    received_message_dropped,
    remote_error
};
```

A synchronous operation that can fail returns `result_t<T>` or
`result_t<void>`. Success is confirmed with an explicit bool
conversion, and on failure, `error_t` is read with `error()` and
`error_code()`. The callback form also delivers the same `result_t`.
Error kind and meaning is owned by the
[common spec](../../32-stream-connector.en.md).

### 5.1 Flow Correlation

An outbound operation the Connector starts generates a UUIDv7
`flow_id` once, with no separate public option. A follow-up operation
started from an inbound callback reuses the current inbound flow, and
once the callback ends, the connector runtime cleans up the current
flow context. The wire format and async context boundary is owned by
[Common Stream Connector §4.2](../../32-stream-connector.en.md) and
[Flow Correlation §6](../../../27-flow-correlation.en.md#6-async-work-and-execution-context).

## 6. Options

`connector_options_t` expresses endpoint, transport,
connect/request/wait timeout, heartbeat, reconnect, send/receive
payload bound, observer and receive message queue bound, TLS
validation, dispatch mode, and compression. The default value and
validation rule follow
[Common Spec §6.1](../../32-stream-connector.en.md).

The Connector metric is delivered to the public sink below. If the sink
isn't configured, only metric recording is skipped, and connector
behavior doesn't change.

```cpp
using connector_metric_attributes_t =
  std::map<std::string, std::variant<std::string, std::int64_t, double, bool>>;

class connector_metric_sink_t {
public:
    virtual ~connector_metric_sink_t() = default;
    virtual void add_counter(
      std::string_view name,
      std::string_view unit,
      std::uint64_t value,
      const connector_metric_attributes_t &attributes) noexcept = 0;
};

struct connector_options_t {
    std::string endpoint;
    transport_t transport = transport_t::tcp;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{30000};
    std::chrono::milliseconds wait_timeout{5000};
    heartbeat_options_t heartbeat;
    reconnect_options_t reconnect;
    std::size_t max_send_payload_size = 64 * 1024;
    std::size_t max_receive_payload_size = 64 * 1024;
    std::size_t max_inbound_observer_notifications = 1024;
    std::size_t max_received_messages = 1024;
    std::size_t max_inbound_observer_payload_preview_bytes = 0;
    bool skip_server_certificate_validation = false;
    dispatch_mode_t dispatch_mode = dispatch_mode_t::manual;
    compression_t compression = compression_t::lz4;
    std::shared_ptr<const compression_codec_t> compression_codec;
    std::shared_ptr<const typed_codec_t> typed_codec; // the default JSON codec if empty
    std::shared_ptr<connector_metric_sink_t> metric_sink;
};
```

`zlink.stream.reconnects`'s name and closed attribute follow
[Common Spec §6.2](../../32-stream-connector.en.md#62-connector-reconnect-instrument).
The application and E2E read the counter from the sink implementation.
The sink is fixed as `noexcept` so it doesn't let an exception escape
its boundary, and a metric processing failure doesn't change send,
request, or connection state.

`options()` returns a copy of the configuration the
[factory](../../../01-glossary.en.md#factory) applied. The value the
getter shows must be the value the actual connect, request, wait,
queue, TLS, and compression paths use — a configuration value not
reflected in behavior isn't exposed.

## 7. Inbound Observer

`observe_inbound(...)` is registered before connection starts and
returns a move-only `inbound_observer_registration_t`. The
registration's `close()` deregisters the observation. The observer's
isolation, payload preview, and overflow behavior follows
[Common Spec §10](../../32-stream-connector.en.md).

## 8. Verification

`test_cpp_stream_connector` verifies the C++ connector's public
behavior. The existence of the per-language contract document and the
test helper interface is verified by
`test_cpp_framework_target_contract`'s `TH-CP-01` gate.
