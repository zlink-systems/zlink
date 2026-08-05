<!-- framework-adapter-nav:start -->
[Spec table of contents](README.en.md) | [Next: C++ exact interface](interfaces/README.en.md)
<!-- framework-adapter-nav:end -->

# C++ System Structure — Package, Registration, And Bootstrap

[Spec table of contents](README.en.md)

> This document owns **how ZLink framework is composed in C++.**
> Package/build target, application host, **DI container**,
> **configuration**, **logging**, lifecycle, and the **registration
> surface** of each capability.
>
> **The meaning and behavior rule of a capability is owned by the
> common spec** — [channel-messaging](../../../08-channel-messaging.en.md),
> [spot-messaging](../../../12-spot-messaging.en.md),
> [MeshNode](../../../13-mesh-node.en.md),
> [stream-session](../../../19-stream-session.en.md),
> [actor-model](../../../14-actor-model.en.md),
> [session-actor-dispatch](../../../20-session-actor-dispatch.en.md),
> [runtime-monitoring](../../../24-runtime-monitoring.en.md),
> [location-runtime](../../../21-location-runtime.en.md),
> [channel-topology](../../../07-channel-topology.en.md).
>
> **The public type and signature is owned by the
> [exact interface per capability](interfaces/README.en.md).**
> HTTP is owned by [60](60-http-hosting.en.md) ·
> [61](61-embedded-http-server.en.md).
> **The internal runtime structure is owned by
> [internals/runtime-architecture](../../../../internals/README.en.md)**
> — it isn't a public contract.

## 1. Product Position

**The C++ framework isn't a binding helper — it's an application
framework.** It provides host, DI, configuration, logging, and
lifecycle together.

**This is decisively different from the other languages.** `.NET`
**borrows** ASP.NET Core, Node borrows NestJS, and Java borrows Spring
Boot. C++ has no such host, so **the framework provides it directly.**
That's why only the C++ document keeps a per-capability spec.

What the framework must provide:

| Axis | Content |
|---|---|
| **Application host** | Starts application bootstrap and manages hosted service, module, and lifecycle. |
| **DI container** | Manages service lifetime and scope and provides dependencies through constructor injection. |
| **Configuration** | Provides application and Framework option as a hierarchical settings model. |
| **Logging** | Selects log level and backend and manages a bounded async queue and rotating file. |
| **HTTP hosting** | Provides an embedded HTTP server, route, and middleware inside the application lifecycle. |
| **zlink messaging** | Provides Channel, Spot, STREAM, and Actor message API. |
| **Handler model** | Registers a handler, dispatches it in the selected execution context, and applies filter. |
| **Observability** | Provides metric, message flow, and health status to the application. |

**The public API is expressed in C++ convention** — RAII, value type,
template, coroutine.

## 2. Package And Build Target

| Target | CMake | Content |
|---|---|---|
| `zlink_framework` | **`zlink::framework`** (STATIC) | Provides Framework core and requires C++20 (`cxx_std_20`). |

**The client connector is a separate product line** — owned by the
[C++ Stream Connector guide](../../../../../cpp/guide/stream-connector/INDEX.en.md).
It doesn't mutually depend on the server framework.

## 3. Application Host

```cpp
class app_t;            // host instance
class app_advanced_t;   // advanced configuration access
class hosted_service_t; // start/stop hook
class module_t;         // capability bundle registration
```

- **`module_t` bundles related registration into one unit.** Used to
  split a large app by capability.
- **Runtime is built at host startup and cleaned up at shutdown.** It
  isn't hidden behind lazy creation
  ([channel-messaging §2](../../../08-channel-messaging.en.md)).

### 3.1 Hosted Service Execution Order

| Stage | Rule |
|---|---|
| **Start** | Starts in registration order |
| **Stop** | **Cleans up in reverse start order** |

### 3.2 Start Failure — Fail-Fast

**If even one service fails during start, the services started so far
are cleaned up in reverse order and the exception is re-thrown.** No
half-started host is left behind.

**Cleanup doesn't fail** (`noexcept`). Because an error during cleanup
must not mask the original failure.

## 4. DI Container

### 4.1 Lifetime

```cpp
enum class service_lifetime_t { singleton, scoped, transient };
```

| Lifetime | Meaning |
|---|---|
| `singleton` | One across the whole host |
| `scoped` | **One per scope** (§4.2) |
| `transient` | A new one every resolve |

### 4.2 Scope Boundary

Creation and cleanup of a `scoped` service is performed by the
Framework at the handler, STREAM session, and object lifecycle
boundary. The application doesn't choose a scope kind or create a
scope directly.

### 4.3 Registration

```cpp
class service_collection_t
{
public:
    template <typename T> service_collection_t &add_singleton ();
    template <typename T, typename... TDependencies>
    requires (sizeof...(TDependencies) > 0) service_collection_t &add_singleton ();
    template <typename T> service_collection_t &add_singleton (std::unique_ptr<T> instance);

    template <typename T> service_collection_t &add_scoped ();
    template <typename T, typename... TDependencies>
    requires (sizeof...(TDependencies) > 0) service_collection_t &add_scoped ();

    template <typename T> service_collection_t &add_transient ();
    template <typename T, typename... TDependencies>
    requires (sizeof...(TDependencies) > 0) service_collection_t &add_transient ();

    template <typename T, typename TFactory>
    service_collection_t &add_factory (
      TFactory factory,
      service_lifetime_t lifetime = service_lifetime_t::transient);
    template <typename T> service_collection_t &add_framework_dependency ();
};
```

- **A dependency is declared as a template argument.** With no
  argument, **a default constructor is required** (statically
  verified).
- **Declaring a handler's dependency with `dependency_list_t<...>`**
  makes the framework inject those types and build the handler.
- **`logger_t<TCategory>` is a framework dependency** — it's
  auto-wired by `add_framework_dependency`.

### 4.4 Resolve

```cpp
class service_provider_t
{
public:
    template <typename T> T &get_required ();                              // fails if absent
    template <typename T> std::optional<std::reference_wrapper<T>> get (); // empty value if absent
};

```

**A handler doesn't receive a service locator.** Only constructor
injection is used.

### 4.5 Error Contract

| Situation | Result |
|---|---|
| **Registering the same type twice** | **Fails at registration time** — doesn't silently overwrite |
| **`get_required` on an unregistered type** | **Fails** |
| `get` on an unregistered type | **Returns an empty value.** Doesn't fail |
| **Resolving a `scoped` service with no scope** | **Fails** — scoped requires a scope |
| **Resolving from a closed provider** | **Fails as a [shutdown](../../../01-glossary.en.md#shutdown) boundary error** |

### 4.6 Lifetime And Cleanup

- **`singleton` is built on the first resolve and reused for the
  host's lifetime.**
- **`scoped` is built on the first resolve in that scope and reused
  within the scope.**
- **When the Framework closes a scope, it cleans up that scope's
  `scoped`/`transient` instances together.**

**A closed provider can't be used again.** Every subsequent resolve
fails.

## 5. Configuration

```cpp
enum class optional_t;      // required/optional distinction
class configuration_model_t;
```

**Configuration sources are merged as a hierarchy.** A source added
later overwrites an earlier one. If a required value is absent, **it
fails before host start.**

## 6. Logging

**The framework provides logging.** It doesn't force an external
logging library.

```cpp
enum class log_level_t { trace, debug, info, warn, error, critical, off };
enum class logging_backend_t { builtin, structured };
enum class logging_overflow_policy_t { drop_debug, drop_oldest, block };

struct log_field_t;    // structured log key-value
struct log_record_t;   // one log entry

struct logging_async_options_t
{
    std::size_t queue_capacity = 8192;
    logging_overflow_policy_t overflow_policy = logging_overflow_policy_t::drop_debug;
};

struct rotating_file_options_t
{
    std::size_t max_file_size = 10 * 1024 * 1024;   // 10 MiB
    std::size_t max_files = 5;
};

template <typename TCategory = void> class logger_t;  // injected through DI
class logger_factory_t;
```

**Async logging's overflow policy is a contract.**

| Policy | Behavior |
|---|---|
| `drop_debug` **(default)** | When the queue fills, **debug and below are dropped first.** Keeps important logs alive |
| `drop_oldest` | Drops the oldest entry |
| `block` | **Blocks the caller.** Doesn't lose a log, but propagates the delay |

**`logger_t<TCategory>` is injected through DI.** Category is
distinguished by type.

## 7. HTTP Hosting

**The framework provides an embedded HTTP server.** The contract is
owned by [60](60-http-hosting.en.md) · [61](61-embedded-http-server.en.md),
and the public type is owned by
[configuration and host](interfaces/02-configuration-host.en.md). Here
only the **rule that hits system structure** is summarized.

### 7.1 Per-Request DI Scope

**One request is one scope.** The route handler and middleware receive
**the same request-scope provider**. When the request ends, that
scope's `scoped`/`transient` instances are cleaned up (§4.6).

### 7.2 Middleware Execution Order

**Middleware is a `before`/`after` pair.** This differs from the
handler filter's `next` delegate approach
([Framework API §8.1](../../../06-framework-api.en.md#81-handler-filter)).

| Stage | Order |
|---|---|
| `before` | **In registration order** |
| Route handler | — |
| `after` | **In reverse order** |

**`after` runs only for the middleware whose `before` ran.**

## 8. Handler Registration And Filter

The handler registration surface and filter contract are owned by
[Channel Messaging §3](interfaces/03-channel-messaging.ko.md#3-handler-registry).
Filter's language-neutral meaning is owned by
[Framework API §8.1](../../../06-framework-api.en.md#81-handler-filter).

## 9. Capability Registration

The registration surface of each capability is owned by the
[exact interface per capability](interfaces/README.en.md).

| Capability | Section |
|---|---|
| Channel | §7 Channel Builder |
| SPOT · Actor | §11 Spot Framework API and Instance Spot registration/call |
| STREAM | §12 Hosted Service and Module |
| HTTP | [60](60-http-hosting.en.md) · [61](61-embedded-http-server.en.md) |
| Monitoring · Location | §13 Configuration and Logging |

**The startup validation item is owned by the common spec** —
[channel-messaging §4](../../../08-channel-messaging.en.md),
[spot-messaging §8](../../../12-spot-messaging.en.md),
[stream-session §7.2](../../../19-stream-session.en.md),
[runtime-monitoring §6](../../../24-runtime-monitoring.en.md).

**C++ turns every violation into a failure before host start.** The
error follows the `result_t`/`framework_exception_t` boundary
convention, not an exception
([common runtime](interfaces/01-common-runtime.ko.md)).

## 10. Regression Test

The regression item for registration and startup validation is owned
by
[regression-test-matrix](../../../../../cpp/internals/regression-test-matrix.en.md).
