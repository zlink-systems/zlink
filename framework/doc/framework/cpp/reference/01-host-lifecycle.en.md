# 01. Host lifecycle

[Reference index](README.en.md)

This category covers the host registration, relocation, shutdown, and status entry points that
`app_t` provides, and the health registration that `app_t::health()` provides. The exact
signatures are owned by the
[Configuration and host exact interface](../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md)
and the
[Monitoring exact interface](../../common/spec/server/languages/cpp/interfaces/08-monitoring.en.md)
(Korean-only).

---

## `app_t::add_zlink_framework` (configuration time)

Registers the framework root with the app once. It is the prerequisite for every other entry in
this reference.

```cpp
zlink::framework::app_t app = zlink::framework::app_t::create();

app.add_zlink_framework([&](zlink::framework::zlink_framework_options_t &options) {
    auto play = options.add_route_mesh("play")
      .listen(5501)
      .set_automatic_routing_id_prefix("play")
      .set_placement_weight(100);
});

return app.run(argc, argv);
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `configure: std::function<void(zlink_framework_options_t &)>` | required | The entry point for every registration — topology, handlers, Location Store, and more |
| `add_zlink_framework<TModule, TArgs...>(args...)` | — | An overload that registers a type implementing `module_t` along with its constructor arguments. `module_t::configure(options)` does the same job as the callback above |

**Completion result.** Registers synchronously with no return value. `app.run(argc, argv)`
validates the configuration before the socket bind, and if it fails, fails startup itself with a
configuration error — a bad configuration never surfaces for the first time while messages are
already being processed.

**When to use.** Every host calls this exactly once. See the topology-discovery category for the
topology and handler registration details of `zlink_framework_options_t`.

---

## `relocate`

Moves the stateful objects (User Spots, Actors) the current host holds to another eligible node.
Call it before planned maintenance or a rolling update.

```cpp
zlink::framework::relocation_options_t options{
    .mode = zlink::framework::relocation_mode_t::rolling_update,
    .target_application_version = 2,
    .deadline = std::chrono::minutes{5},
};

zlink::framework::relocation_result_t result = co_await app.relocate(options);

if (result.outcome == zlink::framework::relocation_outcome_t::relocated) {
    co_await app.shutdown();
}
```

**Options.** The fields of `relocation_options_t` are as follows.

| Field | Default | Meaning |
| --- | --- | --- |
| `mode` | required | `planned_maintenance` (only targets the same application version as the source) or `rolling_update` (only targets the specified version) |
| `target_application_version` | not specified for `planned_maintenance` (uses the source's value); required for `rolling_update` | The target application version. If the combination is invalid, the call is rejected with `std::invalid_argument` before it starts |
| `deadline` | 30 seconds if omitted | The upper bound for waiting on eligible target convergence |
| `wait_cancellation` (`relocate(options, stop_token)`) | none | Cancels only the waiter. Does not cancel a shared operation that has already started |

**Completion result.** If `relocation_result_t::outcome` is `relocated`, every object has finished
moving and the host reaches the `relocated` state (it accepts no new operations but keeps
infrastructure connections). If it is `blocked`, `reason` carries values such as
`target_unavailable`, `store_unavailable`, or `deadline_exceeded`, and the host returns to
`serving` if it still has local objects it was processing.

**When to use.** Use this for zero-downtime relocation before a deployment. To shut down directly
without relocating, call `shutdown` directly. A duplicate call with the same
`relocation_options_t` joins the in-flight operation; a call with different values completes with
`blocked/operation_in_progress`.

---

## `shutdown`

Shuts the host down. It does not start a relocation — call `relocate` first if relocation is
needed.

```cpp
zlink::framework::termination_result_t result =
  co_await app.shutdown(std::chrono::seconds{30});
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `deadline` | 30 seconds | The upper bound for teardown. Exceeding it completes with `force_stopped` |
| `wait_cancellation` | none | Cancels only the waiter |

**Completion result.** `termination_result_t::outcome` is `stopped` (clean teardown) or
`force_stopped` (deadline exceeded or teardown failure). Calling it from `serving` cleans up
remaining application processing and resources; calling it from `relocated` cleans up only the
infrastructure connections. Either way, the host reaches `stopped` once it finishes.

**When to use.** Always call this when shutting down the host. Calling it during `relocating`
finalizes only the atomic relocation unit currently in progress and does not start the rest — a
caller waiting on that relocation receives `blocked/shutdown_requested`.

---

## `run` / `stop` / `request_stop`

Runs the host process and sends it a stop signal.

```cpp
int exit_code = app.run(argc, argv); // blocks and runs the entire host lifecycle

// requesting a stop from another thread or a signal handler
app.request_stop();
```

**Options.** None of the three calls has modifiers.

**Completion result.** `run(argc, argv)` is a blocking call that returns an `int` usable as the
process exit code once the host has fully stopped. Handler exceptions, runtime errors, and signal
shutdowns are all collected by the host, which then closes out the shutdown path. `stop()` starts
an immediate stop, while `request_stop()` only sends a graceful-shutdown signal.

**When to use.** `run` is called once from an ordinary `main()` entry point. `request_stop` is
used from external code such as a signal handler to start host shutdown.

---

## `is_ready` / `set_message_flow_mode` (read/change)

Checks whether the host is ready, and changes the message-flow diagnostics mode while running.

```cpp
bool ready = app.is_ready();
app.set_message_flow_mode(zlink::framework::message_flow_log_mode_t::verbose);
```

**Options.** This entry point has two independent properties.

| Property | Default | Meaning |
| --- | --- | --- |
| `is_ready()` | — | `true` only when `framework_runtime_t::status().state == serving` |
| `message_flow_mode()` / `set_message_flow_mode(...)` | The value registered by `configure_dispatch().message_flow(...)` | The diagnostics detail level while running |

**Completion result.** Both calls are synchronous getters/setters. Even when `is_ready()` is
`false`, `status()` (owned by `framework_runtime_t`, not host-lifecycle — see
observability-diagnostics) can give a more detailed status.

**When to use.** Use `set_message_flow_mode` when you need to raise or lower the diagnostics
detail level only at a specific moment without redeploying. Register the diagnostics starting
value with the `configure_dispatch()` entry in observability-diagnostics.

---

## Health registration (`app_t::health()`, configuration time)

Registers the health checks to expose on host readiness/liveness probes.

```cpp
app.health()
  .add_zlink_runtime_check()
  .add_channel_check("play.api")
  .add_location_check("location-store");

zlink::framework::health_report_t report = app.health().report();
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.add_zlink_runtime_check(name = "zlink.runtime")` | — | Adds a check for the framework runtime's own status |
| `.add_channel_check(name)` | — | Adds a check for the availability of the named channel |
| `.add_location_check(name)` | — | Adds a check for the Location Store connection |
| `.add_stream_endpoint_check(name)` | — | Adds a check for the STREAM listener's status |
| `.add_hosted_service_check(name)` | — | Adds a check for a hosted service's status |
| `.set_status(name, status, message)` | — | Registers a status the application judged directly, under a name |

**Completion result.** `report()` is a synchronous call that returns a `health_report_t`.
`readiness` and `liveness` are judged separately, and `degraded` never blocks `ready()` or
`live()`.

**When to use.** Use this when configuring the status that HTTP `/health`, `/readiness`,
`/liveness` endpoints (e.g. `http().map_health(...)` when using the HTTP hosting extension) or an
external orchestrator will query.

---

See the
[Configuration and host exact interface](../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md)
and the
[Monitoring exact interface](../../common/spec/server/languages/cpp/interfaces/08-monitoring.en.md)
(Korean-only) for the full rationale.
