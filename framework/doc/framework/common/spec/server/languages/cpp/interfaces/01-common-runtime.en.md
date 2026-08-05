# C++ Common Runtime Exact Interface

[C++ exact interface table of contents](README.en.md)

<!-- framework-adapter-nav:start -->
[Spec table of contents](README.en.md) | [Previous: C++ System Structure](../01-system-structure.en.md) | [Next: C++ HTTP Hosting](../60-http-hosting.ko.md)
<!-- framework-adapter-nav:end -->

[Spec table of contents](../../../../../README.en.md)

> This document is the C++ formal public interface contract of ZLink
> Framework.
> This document follows the common Framework policy under
> `framework/doc/framework/common/spec` as the higher standard, and
> designs the framework layer on top of the C++ binding's public
> library surface.

## 1. Contract Standard

`C++` framework doesn't replace the C++ binding. The framework sits on
top of the C++ binding, and uses the typed public API the binding
provides as the internal runtime substrate.

Capability and usability concepts are aligned to the framework common
spec as the standard. That is, app/host, DI scope, handler registry,
channel messaging, `STREAM`, `SPOT`, ActorGateway session relay,
monitoring, and graceful shutdown provide the same model, and the C++
public API only changes its expression to fit C++20 coroutine,
callback, and RAII ownership.

The binding standard follows the documents below.

- [C++ Binding Specification](../../../../../../../../../bindings/doc/spec/cpp/README.en.md)
- [C++ Codec Extension Specification](../../../../../../../../../bindings/doc/spec/cpp/codec.en.md)

The framework public API is placed under the `zlink::framework`
namespace. The installed public header includes only the formal
contract and explicit extension points. The application must be able
to compose the framework without knowing the transport implementation.

## 2. Binding Public Dependency Boundary

The Framework package only depends on the C++ binding's public API.
Only values defined by the framework contract, such as ChannelName,
topic, typed payload, timeout, and lifecycle, appear in the public
handler and client.

The place a user can pass a binding value directly is limited to a
payload boundary the formal signature specifies, such as `message_t`.
No other binding type appears in a framework public signature.

## 3. Header And Namespace

The recommended public header layout is below. The header under
`contracts/*` is the actual public contract owner corresponding to
`.NET`'s `Contracts/*`, and `zlink/framework.hpp` is a facade that lets
a user include the whole framework surface at once. A one-line
`zlink/framework/*.hpp` compatibility wrapper isn't kept.

```text
zlink/framework.hpp
zlink/framework/version.hpp
zlink/framework/contracts/actors/*.hpp
zlink/framework/contracts/channels/*.hpp
zlink/framework/contracts/codecs/*.hpp
zlink/framework/contracts/configuration/*.hpp
zlink/framework/contracts/dispatch/*.hpp
zlink/framework/contracts/errors/*.hpp
zlink/framework/contracts/eventing/*.hpp
zlink/framework/contracts/handlers/*.hpp
zlink/framework/contracts/http/*.hpp
zlink/framework/contracts/locations/*.hpp
zlink/framework/contracts/messaging/*.hpp
zlink/framework/contracts/spots/*.hpp
zlink/framework/contracts/streams/*.hpp
zlink/framework/contracts/timers/*.hpp
zlink/framework/contracts/workers/*.hpp
```

A public header such as `zlink/framework/runtime.hpp` isn't provided.
The public API only exposes contract names a user understands, such as
`app_t`, `request_client_t`, `spot_context_t`.

This structure doesn't mean every `.NET` public interface is moved to
a C++ pure virtual class. The C++ public API can actively use a
concrete facade and value type. But the facade's member, constructor,
and method signature must not expose a runtime implementation type.
Only a user extension point is kept as an abstract interface or
concept contract.

### 3.1 Public Contract Boundary

A C++ public header only defines the type and result a user configures
or calls. Even if a public facade keeps state, the user shouldn't need
to know that state's data structure or processing order.

The public `route_client_t` and `route_send_call_t` provide a typed
call targeting a node and global Spot ID.
[User Spot](../../../../01-glossary.en.md#entry-spot-user-spot-and-instance-spot)
and Instance Spot use the same ID-only call surface, and don't provide
a separate handle/resolver/logical address type. The request family
returns `channel_request_call_t`. The user doesn't pass a target
MeshNode, location owner token, or generation, and the framework
handles routing envelope, location claim, and serializer selection.

An ordinary request receives a typed reply through
`request_to_node(...).timeout(...).submit<TReply>()`. A value set with
`.metadata(key, value)` is snapshotted per the application metadata
contract, and transport detail and correlation state aren't exposed in
the public API.

The underlying transport and remote error envelope convert to the
following public error meaning.

| Underlying error meaning | C++ error kind |
|-------------------|----------------|
| `timed_out`, `timeout` | `deadline_exceeded` |
| `not_connected`, `route_not_connected` | `unavailable` |
| `not_found`, `request_target_not_found`, `handler_not_found` | `not_found` |
| Admission or filter rejection with no typed result | `rejected` |
| Local queue capacity shortage, Message Follow relay queue bound exceeded | `capacity_exceeded` |
| Target queue capacity shortage a remote error envelope reported | `unavailable` |
| `busy` | One of the two lines above depending on owner location. If the underlying error alone can't tell the location, `unavailable` |
| `protocol_error`, `request_protocol_error` | `protocol_error` |

This table applies with the same meaning to both request completion
and error envelope reply.

A DTO message name preferentially uses
`static constexpr const char *packet_name`. Framework handler
registration and the Stream Connector's send, request, and on default
name read this value. A type with no name can use the C++ type name,
but a public sample and formal DTO must have an explicit packet name.
### 3.2 C++ Public Header Constraint

Since an installed header is directly the public surface in C++, the
following rules apply.

- A template header only has type check and public facade forwarding.
- A public class's state only uses a public contract type.
- An optional dependency type, such as JSON, MessagePack, or Protobuf,
  can appear only in that codec extension's public contract.
- A contract test only includes an installed public header.
- A public inline function doesn't manipulate transport state beyond
  public validation and forwarding.

Every framework type is placed under the `zlink::framework` namespace.
Each type's declaration is owned by exactly one category document
specified in the
[exact interface table of contents](README.en.md).

## 4. Common Result, Coroutine, And Message

```cpp
namespace zlink::framework {

template <typename T>
class result_t {
public:
    static result_t success(T value);
    static result_t failure(
      framework_error_kind_t kind,
      std::string message);
    bool has_value() const noexcept;
    explicit operator bool() const noexcept;
    const T &value() const;
    T &value();
    const framework_exception_t *error() const noexcept;
    framework_error_kind_t error_kind() const;
};

template <>
class result_t<void> {
public:
    static result_t success();
    static result_t failure(
      framework_error_kind_t kind,
      std::string message);
    bool has_value() const noexcept;
    explicit operator bool() const noexcept;
    void value() const;
    const framework_exception_t *error() const noexcept;
    framework_error_kind_t error_kind() const;
};

template <typename T>
class task_t {
public:
    struct promise_type {
        task_t get_return_object();
        std::suspend_never initial_suspend() noexcept;
        std::suspend_never final_suspend() noexcept;
        void unhandled_exception();
        void return_value(result_t<T> result);

        template <typename U>
        void return_value(U &&value);
    };

    explicit task_t(result_t<T> result);
    task_t(task_t &&) noexcept = default;
    task_t &operator=(task_t &&) noexcept = default;
    task_t(const task_t &) = delete;
    task_t &operator=(const task_t &) = delete;
    ~task_t() = default;
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> continuation);
    T await_resume();
    const result_t<T> &result() const;
};

template <>
class task_t<void> {
public:
    struct promise_type {
        task_t get_return_object();
        std::suspend_never initial_suspend() noexcept;
        std::suspend_never final_suspend() noexcept;
        void unhandled_exception();
        void return_void() noexcept;
    };

    explicit task_t(result_t<void> result);
    task_t(task_t &&) noexcept = default;
    task_t &operator=(task_t &&) noexcept = default;
    task_t(const task_t &) = delete;
    task_t &operator=(const task_t &) = delete;
    ~task_t() = default;
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> continuation);
    void await_resume();
    const result_t<void> &result() const;
};

class message_t {
public:
    message_t() = default;

    template <typename TValue>
    static message_t from(TValue value);

    template <typename TValue>
    TValue decode() const;

    bool encoded() const noexcept;
    bool empty() const noexcept;
};

} // namespace zlink::framework
```

## 5. Serialization

The Framework uses a typed JSON serializer as its default path. The
handler and messaging API receive a payload type, and the application
doesn't handle a registry, type-erased pointer, encoder callback, or
raw dispatch table. A payload that can't be expressed as JSON selects a
codec extension package through `options.codecs().use(...)`. The
extension package's registry wiring and payload conversion are a
runtime-internal contract and aren't exposed in the application public
header. Even when the Framework, connector, and HTTP client change
codec, the handler's and client's typed API doesn't change.

```cmake
target_link_libraries(app PRIVATE zlink::cpp)

# Add only when Protobuf is needed.
target_link_libraries(app PRIVATE zlink::framework_codec_protobuf)
```

## 6. C++-Specific Contract

### 6.1 Backpressure

SPOT and STREAM backpressure is only observed through the public
**call object, timeout, and result error kind**.

- **An application handler isn't given an API that directly controls
  the framework queue.**
- **The default policy isn't an unlimited queue.** Queue bound, submit
  timeout, and overflow policy are closed by framework runtime
  configuration, and **exceeding the bound returns a failed result.**
- The error kind of an exceeded bound differs by operation family and
  queue location. It follows the §error mapping table above and
  [Spot Messaging §5.3](../../../../12-spot-messaging.en.md) — it
  isn't uniformly `capacity_exceeded`. Source-local saturation of
  one-way/send is `deadline_exceeded`, a request's local queue
  saturation is `capacity_exceeded`, and a remote queue saturation is
  `unavailable`.

### 6.2 Handler Filter

A filter reads the current dispatch kind and public metadata through
`handler_filter_context_t`. The descriptor and raw message storage are
kept Framework-internal. The filter doesn't return a result, and can't
build or change a request reply.

Not calling `next()` only ends the current handler for send and
Classic Fanout. A request completes as `rejected`. `next()` can be
called only once, and a second call is an `invalid_operation` error.

The filter's registration order, `next` meaning, and scope are owned by
[Framework API §8.1](../../../../06-framework-api.en.md#81-handler-filter).

### 6.3 Public Surface Boundary

The handler public contract is owned by `contracts/handlers/*`. The
application only uses the handler signature, public metadata, and
result, and doesn't control handler lookup, DI resolve, or serializer
execution order.


### 6.4 Timer Execution

Timer callback, packet, and Actor turn are ordered in the same owner's
serial execution queue. The application only uses logical timer
registration and callback metadata.

**A handler that's CPU-bound or might block is handed off to the
Framework runtime's offload execution** (§7.3 worker).

### 6.5 Actor Gateway Decision

| Item | Decision |
|------|------|
| **`actor_ref_t` public shape** | A C++ value type holding node routing id, actor id, and **generation** |
| **Session creation** | The session implementation is **resolved from DI.** The handler registry callback is kept only as a low-level extension surface |
| **Remote ActorGateway** | The application only sees the `actor_ref_t` and session actor surface |
| **Actor factory duplicate policy** | A duplicate same actor id is reported as **`already_exists`**, and an actor id/type mismatch as **`type_mismatch`** |

**`actor_ref_t`'s `node_rid`/`actor_id`/`generation` is preserved
across the bind/relay/push round trip.**
**Local actor relay and remote actor relay use the same public
surface.**

## 7. Public Type Catalog

**This section fills in the public type not covered by the sections
above.** A `*_state_t`/`*_snapshot_t` not here is **runtime-internal
state** and isn't a public contract.

### 7.1 Dispatch Error Contract

A dispatch failure doesn't make a separate event type — it's expressed
as [Monitoring §2](08-monitoring.en.md#2-message-flow-observation)'s
`message_flow_event_t`. The closed value and conditional field rule of
`surface`, `message_kind`, `reason`, `action` is owned by
[Message Flow Tracing §3-§4](../../../../26-message-flow-tracing.en.md).

### 7.2 Dispatch Execution Policy

`handler_execution_t` distinguishes how a handler executes. The exact
declaration of dispatch diagnostics, message-flow, and error event is
owned by the [Monitoring interface](08-monitoring.en.md).

### 7.3 Worker

```cpp
template <typename TResult> class worker_call_t
{
public:
    using executor_t = std::function<task_t<TResult>(
      std::stop_token)>;

    worker_call_t() = default;
    explicit worker_call_t(executor_t executor);
    worker_call_t &timeout (std::chrono::milliseconds value);
    task_t<TResult> submit ();
    task_t<TResult> yield ();
};

class worker_options_t {
public:
    std::size_t min_threads() const noexcept;
    worker_options_t &min_threads(std::size_t value);
    std::size_t max_threads() const noexcept;
    worker_options_t &max_threads(std::size_t value);
    std::chrono::milliseconds idle_timeout() const noexcept;
    worker_options_t &idle_timeout(std::chrono::milliseconds value);
    std::size_t max_queue_length() const noexcept;
    worker_options_t &max_queue_length(std::size_t value);
};
```

**A worker is work that runs outside a spot/session execution
context.** The rule for resuming completion in the original execution
context is owned by
[Async Execution Policy](../../../../05-async-execution-policy.en.md).
The worker function is passed a `std::stop_token` combining timeout,
host shutdown, and caller cancellation. `submit()` is a terminal that
doesn't wait for a result, and `submit()` keeps the current turn and
waits for the result. `yield()` returns that turn and waits for the
result only in a `SpotWide` User Spot's or Instance Spot's shared turn.
In a different execution context, it completes with
`invalid_operation` without submitting the worker or returning the
turn.
`worker_options_t`'s min/max thread count, idle timeout, and queue
bound are set only before host start.

### 7.4 Error Boundary

An API that returns synchronous validation and an explicit result
object returns failure as `result_t<T>`. An async call's `submit()`
throws `framework_exception_t` carrying the same error information on
failure. The application's error branch uses `kind()`. `code()` adds
diagnostic information when there's a platform cause such as timeout
or transport, but doesn't replace the common error classification.


The same Spot's dispatch serialization and `yield()`'s allowed scope
are owned by
[Stage Wrapper §3](../../../../17-stage-wrapper-on-spot.en.md) and
[Async Execution Policy](../../../../05-async-execution-policy.en.md).

---
<!-- framework-adapter-nav:bottom:start -->
[Spec table of contents](README.en.md) | [Previous: C++ System Structure](../01-system-structure.en.md) | [Next: C++ HTTP Hosting](../60-http-hosting.ko.md)
<!-- framework-adapter-nav:bottom:end -->
