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

`connect` returns `error_code_t::validation_failed` or `error_code_t::configuration_error`
for option validation under [Common Spec §6.3](../../32-stream-connector.en.md#63-option-validation).

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
std::size_t received_count(std::string_view packet_name) const; // the received count per packet name.
template <typename TMessage> std::size_t received_count() const; // derives the name from the type.

result_t<void> connect();                                  // waits for the connection result in the current call.
void connect(std::function<void(result_t<void>)> callback); // receives the connection result as a callback.
result_t<void> close();                                    // waits for the close result in the current call.
void close(std::function<void(result_t<void>)> callback);   // receives the close result as a callback.
result_t<void> dispatch();                                 // runs one pending callback of Manual mode.
```

`received_count` returns a count per packet name. Counting and reset follow
[Common Spec §10](../../32-stream-connector.en.md#10-receive-message-queue).

A received message is a `message_t<TPayload>`. The `on<T>` handler and
the `wait_for` family handle this type.

```cpp
template <typename TPayload>
struct message_t {
    std::string packet_name;
    TPayload payload;                          // the payload decoded with the typed codec
    metadata_t metadata;
    std::optional<std::string> actor_id;       // the counterpart bound Actor; empty for a frame without a slot (common spec §5.6)
};
```

A push callback is registered with `on<T>(...)`. The relationship of
`dispatch()` and `wait_for` to dispatch follows
[Common Spec §7](../../32-stream-connector.en.md#7-dispatch-mode).

**A handler registration returns a `subscription_t` that deregisters
it** ([Common Spec §7](../../32-stream-connector.en.md#7-dispatch-mode)).
The push handler and the error/disconnect/connection state handlers all
return the same type.

```cpp
class subscription_t {   // move-only; the destructor releases a remaining registration
public:
    void unsubscribe();  // calling it twice isn't treated as an error
    bool active() const;
};

template <typename TMessage>
subscription_t on(std::function<void(const message_t<TMessage>&)> callback);
template <typename TMessage>
subscription_t on(std::string packet_name,
                  std::function<void(const message_t<TMessage>&)> callback);
subscription_t on_error(std::function<void(const error_t&)> callback);
subscription_t on_disconnected(std::function<void(std::optional<stream_close_reason_t>)> callback);
subscription_t on_connection_state_changed(
    std::function<void(const connection_state_changed_t&)> callback);

// The Actor handles bound right now (common spec §5.6). The application never creates one.
std::vector<std::shared_ptr<actor_t>> actors() const;
std::shared_ptr<actor_t> actor(std::string_view actor_id) const;   // nullptr when there is none
struct request_sending_context_t {
    const std::string request_packet_name;
    const std::optional<std::string> actor_id;
    void set_metadata(std::string key, std::string value);
};
struct reply_received_context_t {
    std::string request_packet_name;
    std::optional<std::string> actor_id;
    bool succeeded;
    std::optional<packet_t> reply;
    std::optional<error_t> error;
    std::chrono::milliseconds elapsed;
};
subscription_t on_request_sending(
    std::function<void(request_sending_context_t&)> callback);
subscription_t on_reply_received(
    std::function<void(const reply_received_context_t&)> callback);
subscription_t on_actor_bound(std::function<void(const std::shared_ptr<actor_t>&)> callback);
subscription_t on_actor_unbound(std::function<void(const std::shared_ptr<actor_t>&)> callback);
```

```cpp
class actor_t {                 // one bound Actor. The connector owns it and closes it on the unbound announcement
public:
    const std::string& actor_id() const noexcept;
    bool is_bound() const noexcept;            // false after the unbound announcement

    template <typename TMessage>
    send_call_t send(const TMessage& message);       // carries this Actor's slot
    template <typename TRequest>
    request_call_t request(const TRequest& request);
    template <typename TMessage>
    subscription_t on(std::function<void(const message_t<TMessage>&)> callback);   // only messages whose counterpart is this Actor
    template <typename TMessage>
    subscription_t on(std::string packet_name,
                      std::function<void(const message_t<TMessage>&)> callback);
};
```

A deregistered handler isn't run by a later `dispatch()`.

**The close reason's read surface is the `close_reason()` method.** It
returns an empty value if the connector hasn't disconnected yet, and a
reconnect doesn't clear it — it keeps the last close's reason. The
`on_disconnected` handler taking the reason as an argument is a surface
added on top of that read surface. The close reason's closed value and
meaning is owned by
[Common Spec §6.2](../../32-stream-connector.en.md#62-close-reason).
enum values 1-6 are the same as the `session-closing` wire value, but
the codec explicitly converts them and doesn't cast the enum to an
integer to build a frame.

## 3. Send And Wait Builder

A typed `send` and `request` decide the packet name from the message
type. A raw packet overload is also provided. Each call returns a
builder, and executes only once the terminator `submit` is called.

The **means of attaching a packet name to a type** that
[Common Spec §5](../../32-stream-connector.en.md#5-packet-model)
requires is a static member. C++ has no attribute or annotation, so the
name goes on the payload type.

```cpp
struct order_changed_t {
    static constexpr const char* packet_name = "order.changed"; // the packet name attached to the type
    // ...
};
```

`connector_options_t::name_resolver` (§6) prioritizes this static member
and uses the type's simple name when it is absent. **A mangled name the
compiler builds is never used as a packet name** — its value changes
with the build environment, so the server would fail to find the handler
for the same type. A name the caller states with the builder's
`packet_name(...)` takes priority over both.

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
template <typename TReply>
result_t<TReply> submit(); // waits for a matching-correlation reply and decodes it as TReply.
template <typename TReply>
void submit(std::function<void(result_t<TReply>)> callback);
```

A single push wait is handled by `wait_call_t<TMessage>`. The predicate
and the return value handle `message_t<TMessage>`, not the payload (§2).

```cpp
template <typename TMessage>
wait_call_t<TMessage> wait_for();
template <typename TMessage>
wait_call_t<TMessage> wait_for(std::string packet_name);

wait_call_t<TMessage>& where(std::function<bool(const message_t<TMessage>&)> predicate);
wait_call_t<TMessage>& timeout(std::chrono::milliseconds timeout);
result_t<message_t<TMessage>> submit(); // consumes and decodes one matching unread packet.
void submit(std::function<void(result_t<message_t<TMessage>>)> callback);
```

The one-way `submit()` doesn't return a result. Since the C++ connector
core keeps the common contract's no-exception/no-coroutine boundary, a
new `task_t` isn't introduced for this terminal. A send failure is
reported through the existing connector error event. Request and wait
keep the existing result type and also provide a callback completion
path.

Typed `send`, `request`, `on`, and `wait_for` all use the single codec
put in `connector_options_t::typed_codec` — the injection point of
[Common Spec §5.4](../../32-stream-connector.en.md#54-codec) — together. If not specified,
the JSON codec is used. Protobuf, MessagePack, and a user codec
extension provide a `typed_codec_t` implementation, put into options
once when building a connector. A public API for registering a codec
per message type, or choosing a codec per send/request operation, isn't
provided. A Raw encoded payload uses the codec number already recorded
in the payload as is, for external protocol interworking.

## 4. Test Wait Interface

The behavioral contract is owned by
[Common Spec §10.1](../../32-stream-connector.en.md).

### 4.1 Push Observation — Connector Method

`expect_none` and `wait_for_sequence` are `connector_t` methods in the
same spot as `wait_for`. Both an overload that decides the packet name
from the type name and an overload where the caller specifies the
packet name are provided.

```cpp
template <typename TMessage>
expect_none_call_t<TMessage> expect_none();
template <typename TMessage>
expect_none_call_t<TMessage> expect_none(std::string packet_name);
expect_none_call_t<packet_t> expect_none(std::string packet_name);

template <typename TMessage>
wait_for_sequence_call_t<TMessage> wait_for_sequence();
template <typename TMessage>
wait_for_sequence_call_t<TMessage> wait_for_sequence(std::string packet_name);
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

`wait_for_sequence` returns `std::vector<message_t<TMessage>>`.
Sequence observation and failure follow
[Common Spec §10.1](../../32-stream-connector.en.md#101-test-wait-surface).

```cpp
auto result = connector.wait_for_sequence<order_changed_t>()
                .expect([](const auto& message) { return message.payload.status == status_t::paid; })
                .expect([](const auto& message) { return message.payload.status == status_t::shipped; })
                .timeout(std::chrono::seconds(2)) // the overall time limit satisfying both predicates.
                .submit();
```

Both builders provide the `result_t`-returning form of `submit()` and
the `submit(...)` form that takes a callback. These surfaces fail
with `error_code_t::validation_failed`. A status-only method isn't
provided. Since status is a payload field, a single observation is
expressed as
`wait_for<T>().where([](const auto& m) { return m.payload.status == …; })`,
and a sequence observation as `wait_for_sequence<T>().expect(...)`. Domain REST polling is the
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
    remote_error
};
```

C++ uses both branches of
[Common Spec §9.2](../../32-stream-connector.en.md#92-delivery--the-receiver-must-be-able-to-read-the-code).

**The connector core delivers by value and throws nothing**, because a
game engine build with exceptions disabled uses the core as is. A
synchronous operation that can fail returns `result_t<T>` or
`result_t<void>`. Success is confirmed with an explicit bool
conversion, and on failure, `error_t` is read with `error()` and
`error_code()`. The callback form also delivers the same `result_t`.

```cpp
struct error_t {
    error_code_t code;    // where the code is read
    std::string message;
};
```

**E2E and tooling use a throwing adapter.** The adapter is a separate
CMake target, `zlink::stream_connector_throwing`, and it doesn't change
the core contract.

```cpp
#include <zlink/stream_connector_throwing.hpp>

namespace zlink::stream_connector_throwing
{
class stream_connector_error : public std::runtime_error
{
public:
    zlink::stream_connector::error_code_t code() const noexcept; // where the code is read
};

template <typename T> T value_or_throw(result_t<T> result); // throws stream_connector_error on failure
inline void value_or_throw(result_t<void> result);
}
```

Both branches deliver the same `error_code_t` value, and whichever is
used, the receiver reads which of the 13 codes in common spec §9 it is.
Error kind and meaning is owned by the
[common spec](../../32-stream-connector.en.md).

### 5.1 Request Hooks

`on_request_sending`/`on_reply_received` are the two hooks of
[Common Spec §5.7](../../32-stream-connector.en.md#57-request-hooks) and return `subscription_t`. A hook
failure is reported through `on_error`.

## 6. Options

`connector_options_t` expresses endpoint, transport,
connect/request/wait timeout, heartbeat, reconnect, send/receive
payload bound, TLS
validation, dispatch mode, and compression. The default value and
validation rule follow
[Common Spec §6.1](../../32-stream-connector.en.md), and §1 fixes the
validation timing.

The **unlimited reconnect** that
[Common Spec §6](../../32-stream-connector.en.md#6-connection-lifecycle)
requires is expressed as an empty `std::optional<int>`.

The **unspecified transport** that
[Common Spec §3.1](../../32-stream-connector.en.md#31-endpoint-scheme--transport)
requires is expressed as an empty `std::optional<transport_t>`. An empty value
lets the endpoint scheme decide the transport; a value present is the stated
transport, and a mismatch with the endpoint scheme is
`error_code_t::configuration_error`. A fixed default would make a configuration
given only a `ws://` endpoint indistinguishable from one stating
`transport_t::tcp`.

```cpp
struct reconnect_options_t {
    bool enabled = true;
    std::chrono::milliseconds initial_delay{250};
    std::chrono::milliseconds max_delay{5000};
    double backoff_factor = 2.0;
    std::optional<int> max_attempts = 3; // an empty value means unlimited; otherwise it must be positive
};

struct connector_options_t {
    std::string endpoint;
    std::optional<transport_t> transport;  // an empty value lets the endpoint scheme decide the transport.
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{30000};
    std::chrono::milliseconds wait_timeout{5000};
    heartbeat_options_t heartbeat;
    reconnect_options_t reconnect;
    std::size_t max_send_payload_size = 64 * 1024;
    std::size_t max_receive_payload_size = 64 * 1024;
    bool skip_server_certificate_validation = false;
    dispatch_mode_t dispatch_mode = dispatch_mode_t::manual;
    compression_t compression = compression_t::lz4;
    std::shared_ptr<const compression_codec_t> compression_codec;
    std::shared_ptr<const typed_codec_t> typed_codec;         // the codec injection point of common spec §5.4; the default JSON codec if empty
    std::shared_ptr<const packet_name_resolver_t> name_resolver; // the name resolver injection point of common spec §5.4; §3's default rule if empty
};

class packet_name_resolver_t {
public:
    virtual ~packet_name_resolver_t() = default;
    virtual std::string resolve(std::string_view type_name) const = 0;
};

```

`options()` returns a copy of the configuration the
[factory](../../../server/00-foundation/02-glossary.en.md#factory) applied. The value the
getter shows must be the value the actual connect, request, wait,
queue, TLS, and compression paths use — a configuration value not
reflected in behavior isn't exposed.

## 7. Engine adapters

The Unreal plugin, the Godot GDExtension and the Axmol adapter own a `connector_t` as a private
implementation and expose a surface shaped for the engine's types and thread rules. Receiving and
requesting take the same shape as in the other connectors: a push is received by registering a packet
name together with a callback, and a request receives its reply at the call. All three follow these two
rules.

- **Callbacks and delegates run only on the engine main thread (the request sending hook runs in the request call context per Common Spec §5.7).** The adapter queues core
  callbacks and delivers them from the `dispatch` the engine calls every frame or through the main
  thread dispatcher the application registered (Godot `set_main_thread_dispatcher`, Axmol
  `set_axmol_thread_dispatcher`).
- **Results arrive through a callback given per call.** A push is
  registered like the core `on`, with the packet name and the callback together (Unreal
  `On(PacketName, Delegate)`, Godot and Axmol `on(packet_name, callback)`), and the registration returns a
  handle that releases it. A request takes its completion callback at the call and delivers that
  request's reply or failure to it (Unreal `RequestJson(..., OnCompleted)`, Godot and Axmol
  `request_json(..., callback)`).

- Adapters exchange JSON text and have no payload type, so the packet name is always explicit.
- Request hooks (Common Spec §5.7) are Unreal `OnRequestSending`/`OnReplyReceived` delegates and
  Godot/Axmol `on_request_sending`/`on_reply_received` callbacks, each returning a release handle. The
  reply hook is delivered on the main thread like other results.
- An exception thrown by an adapter callback, delegate, or request hook is recorded in the engine's error log at the
  adapter boundary and doesn't change the request result or the delivery of other callbacks.

## 8. Verification

`test_cpp_stream_connector` verifies the C++ connector's public
behavior. The existence of the per-language contract document and the
test helper interface is verified by
`test_cpp_framework_target_contract`'s `TH-CP-01` gate.
