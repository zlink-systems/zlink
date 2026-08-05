---
title: "18. DI Container · C++"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.en.md) | [Previous: 3. Core Concepts](03-concepts.en.md) | [Next: 19. Configuration](19-configuration.ko.md)
<!-- framework-adapter-nav:end -->

# 18. DI Container

> **The document that owns this chapter's contract** — covered by
> [C++ configuration and host public contract](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md).
> This chapter explains registration and resolution for the C++-only built-in DI container.

The framework has a built-in ASP.NET Core-style DI (dependency injection) container.
Register services with `service_collection_t`, and pull them out with `service_provider_t`.
A handler only has to declare `dependency_types` and the framework auto-injects the
constructor arguments.

## 1. Three Lifetimes

| Lifetime | Registration | Instance count |
|------|------|------------|
| **singleton** | `add_singleton<T>()` | 1 for the whole app |
| **scoped** | `add_scoped<T>()` | 1 per execution context (scope) |
| **transient** | `add_transient<T>()` | A new instance on every resolve |

`service_scope_kind_t` is an enum that distinguishes the purpose of a scope the Framework
creates. The application never creates a scope directly. The Framework creates and cleans
up the right scope at handler-dispatch, STREAM-session, and Spot-activation boundaries. An
Actor payload is processed as a member function of its containing Spot, so there's no
separate Actor handler registration surface or public handler class.

| Scope kind | Lifetime scope |
|-----------|----------|
| `handler_invocation` | The handler call that processes one HTTP request |
| `stream_session` | One stream connection's lifetime |
| `spot_activation` | Spot activation |
| `entry_spot` | Entry Spot |
| `actor_creation` | Actor creation |

## 2. How To Register

### Registering With A Default Constructor

```cpp
// A type that can be constructed with T()
options.services ().add_singleton<season_store_t> ();
options.services ().add_transient<request_counter_t> ();
```

### Registering With Dependency Injection — The Container Resolves Constructor Arguments

```cpp
// add_singleton<T, Dep1, Dep2, ...>() -- requires a constructor T(Dep1&, Dep2&, ...)
options.services ()
    .add_singleton<bingo_room_allocator_t> ()
    .add_singleton<agent_availability_directory_t> ()
    .add_singleton<agent_assignment_service_t,
                   bingo_room_allocator_t,
                   agent_availability_directory_t> ();
//                 ^ the constructor receives both deps by reference
```

Constructor-signature rule: **dependencies must be received as `T&` references.** No
values, no pointers.

```cpp
class agent_assignment_service_t
{
  public:
    // The container calls this constructor
    agent_assignment_service_t (bingo_room_allocator_t &allocator,
                                agent_availability_directory_t &availability)
        : _allocator (allocator), _availability (availability) {}
};
```

### Registering A Pre-Built Instance

```cpp
auto topology = std::make_unique<sample_topology_t> (config);
options.services ().add_singleton<sample_topology_t> (std::move (topology));
```

Use this for objects that need external initialization -- a topology built by parsing
config, an external client that needs a connection string.

### Registering With A Factory Lambda

```cpp
options.services ().add_factory<http_client_t> (
    [] (zlink::framework::service_provider_t &provider) {
        auto &config = provider.get_required<connection_config_t> ();
        return std::make_unique<http_client_t> (config.base_url, config.timeout);
    },
    zlink::framework::service_lifetime_t::singleton);
```

Use this for complex initialization logic that's hard to express through constructor
injection.

## 3. Automatic Handler Injection

Declaring `dependency_types` on a channel/HTTP handler gets it constructor injection within
the dispatch scope. There's no need to separately call `add_transient<T>()`. Spot packet and
Actor payload handlers are Spot member functions, so they aren't registered as DI handler
classes. A timer handler, being a separate class, is created once per Spot activation and
reused across timer ticks in that same activation. A timer handler's `dependency_types` is
also resolved within the Spot activation scope.

```cpp
class create_game_http_handler_t
{
  public:
    using request_type = create_game_http_req_t;
    using reply_type   = create_game_http_res_t;
    static constexpr const char *topic_name = "CreateGame";

    // 1. Declare the types to depend on, in order
    using dependency_types =
      zlink::framework::dependency_list_t<
        zlink::framework::request_client_t,
        zlink::framework::logger_t<create_game_http_handler_t>>;

    // 2. The constructor receives them in declaration order
    explicit create_game_http_handler_t (
        zlink::framework::request_client_t &client,
        zlink::framework::logger_t<create_game_http_handler_t> &logger)
        : _client (client), _logger (logger) {}

    create_game_http_res_t handle (const create_game_http_req_t &request);

  private:
    zlink::framework::request_client_t &_client;
    zlink::framework::logger_t<create_game_http_handler_t> _logger;
};
```

Because a handler is created as a new instance per request (transient), use an injected
service reference only within the handler's own lifetime.

## 4. Built-In Framework Services

Services the framework registers when the app runs, or auto-registers while processing
`dependency_types`. Put the type you need in `dependency_types` to receive it via
constructor injection.

| Service | Description |
|--------|------|
| `request_client_t` | Sends a channel request — `request(mesh, channel, msg).submit<TReply>()` |
| `logger_t<TOwner>` | A logger tagged with the owning type's name — `_logger.info(...)` |
| `session_actor_manager_t` | Creates, looks up, and binds Actors from a stream session |
| `logger_factory_t` | `create("category")` — for when the category is decided dynamically (`create<TCategory>()` is type-name-based) |

`logger_t<T>` auto-tags the log source name with `T`'s type name — so even an app with
many handlers can tell log sources apart.

## 5. Pulling Directly From `hosted_service_t`

An app-lifecycle service (`hosted_service_t`) pulls directly from the container at
`start(service_provider_t &services)`. The service it needs must already be registered.

```cpp
class season_scheduler_t : public zlink::framework::hosted_service_t
{
  public:
    void start (zlink::framework::service_provider_t &services) override
    {
        // Pulled from the container at start -- null before this
        _store = &services.get_required<season_store_t> ();
        _worker = std::thread ([this] { run_schedule (); });
    }
    void stop () noexcept override
    {
        _running = false;
        if (_worker.joinable ()) _worker.join ();
    }

  private:
    season_store_t *_store = nullptr;
    std::atomic<bool> _running{true};
    std::thread _worker;
};
```

## 6. Lifetime Selection Guide

| Situation | Recommended lifetime |
|------|----------|
| A pure config or read-only object with no shared state (topology, config) | **singleton** |
| Infrastructure reused across the whole app, like a connection or client | **singleton** + an internal thread-safe implementation |
| State that must be isolated per request (transaction, per-request context) | **scoped** |
| A channel/HTTP handler — needs a new instance per request | **transient** (auto-registered via dependency_types) |
| Mutable domain state (a game room, a conversation's state) | **Not DI — managed by SPOT** ([Chapter 6](06-spot.en.md)) |

## 7. Watch For Lifetime Mismatches — Captive Dependency

**A singleton must not be injected with a scoped/transient service.** A singleton lives for
the whole app, so referencing a shorter-lived service can leave a scoped object referenced
after its scope ends, and a transient object's intent -- creating a fresh one per request --
gets captured by the singleton and disappears.

```cpp
// Wrong -- a singleton references a transient
options.services ()
    .add_transient<conversation_context_t> ()
    .add_singleton<support_service_t, conversation_context_t> ();
//   ^ the singleton "captures" the transient
//     it ends up holding onto the reference to whichever conversation_context_t it was
//     first injected with, forever

// Correct
options.services ()
    .add_singleton<conversation_context_t> ()     // If it's safe to share, make it a singleton
    .add_singleton<support_service_t, conversation_context_t> ();
```

Rule: **the lifetime of the service you're registering must not exceed the lifetime of the
dependency it injects.**

## 8. Concurrent Access To A Singleton Service

A singleton is accessed concurrently by multiple threads in the worker pool. If it has
mutable state, it absolutely needs its own synchronization.

```cpp
class agent_availability_directory_t
{
  public:
    void set_available (const std::string &agent_id, bool available)
    {
        std::lock_guard lock (_mutex);   // Protects concurrent writes
        _availability[agent_id] = available;
    }

    bool is_available (const std::string &agent_id) const
    {
        std::shared_lock lock (_mutex);  // Allows concurrent reads
        auto it = _availability.find (agent_id);
        return it != _availability.end () && it->second;
    }

  private:
    mutable std::shared_mutex _mutex;
    std::unordered_map<std::string, bool> _availability;
};
```

A read-only singleton (topology, a config struct) is safe with no lock at all if it exposes
only `const` methods.

## 9. Related Documents

- The formal contract: [C++ configuration and host public contract](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md)
- Reading config values: [19. Configuration](19-configuration.ko.md)
- The list of injectable types: [13. Key Type Usage Index](13-interface-catalog.en.md)
