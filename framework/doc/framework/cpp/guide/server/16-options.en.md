---
title: "16. Options — Setting List And Defaults · C++"
---

<!-- framework-adapter-nav:start -->
[Guide Home](../../../index.ko.md) | [Previous: E2E Testing](15-e2e-testing.ko.md) | [Next: Where To Use ZLink](17-alternative.en.md)
<!-- framework-adapter-nav:end -->

# 16. Options — Setting List And Defaults

> **The document that owns this chapter's contract** — covered by
> [C++ configuration and host public contract](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md).
> This chapter organizes that surface as a list, showing what you can set and what happens
> when you don't. Reading values from a config file is covered by
> [19. Configuration](19-configuration.ko.md).

This chapter gathers **what you can set and what happens if you don't.** What each option
changes is explained by that feature's own chapter — here we look at where it lives and its
default.

## 1. Where Settings Apply

The same setting has a different scope depending on where you specify it.

| Location | Scope | When it can change |
| --- | --- | --- |
| Root `options` | Process-wide defaults | Only before `app.run ()` |
| A builder | That single node / channel / STREAM node | Only before `app.run ()` |
| A runtime option | Part of an already-running value | While running (§7) |

```cpp
auto app = app_t::create ();
app.add_zlink_framework ([] (zlink_framework_options_t &options) {
    // (1) Root -- applies to every payload in this process.
    options.codecs ().use (protobuf_codec_t::default_instance ());
    options.set_default_request_timeout (std::chrono::seconds (30));

    // (2) Builder -- applies only to this single node.
    auto mesh = options.add_route_mesh ("play");
    mesh.listen (node.mesh_endpoint)
      .set_routing_id (zlink::routing_id_t::from (std::string ("play")))
      .set_spot_limit (2000);
    mesh.channel_name ("room").server ();
});
return app.run (argc, argv);
```

No surface calls a builder again after `app.run ()`. An invalid combination isn't deferred
until the first call — it's **blocked by an exception at startup.**

## 2. Root Options

| Option | What it sets | Default |
| --- | --- | --- |
| `codecs ().use (...)` | Payload serialization format | Built-in JSON |
| `set_default_request_timeout (...)` | The cap on waiting for a request reply | 30 seconds |
| `set_message_follow_duration (...)` | How long a message keeps following a target that's relocating | 30 seconds |
| `handlers ()` | Handler group registration | — |
| `metadata ()` | Metadata propagation policy | — |
| `configure_dispatch ()` | Diagnostics level and message flow (§4) | `errors_only` |
| `configure_inbound_dispatch ()` | Host-wide receive cap (§3.2) | Auto-calculated |
| `configure_locations ()` | Location store behavior (§5) | The §5 table |
| `add_location_store (...)` | The location-resolution store | Single-node configuration if omitted |
| `services ()` | DI registration ([18. DI Container](18-di-container.ko.md)) | — |

`set_default_request_timeout` **rejects anything at or below 0.** An invalid value throws
`framework_exception_t` at startup.

## 3. MeshNode Options

Specified on the builder that `add_route_mesh (name)` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `listen (endpoint)` | The address other nodes connect to | Must be specified |
| `set_bind_host` · `set_advertise_host` | Splitting the bind address from the advertised address | Same as the bind address |
| `set_routing_id (...)` | This node's identifier | Auto-generated |
| `set_object_role (...)` | Object role — whether spots/actors place here | Doesn't place |
| `set_placement_weight (int)` | Selection weight for new object placement | 100 |
| `set_actor_limit` · `set_spot_limit` | This node's capacity cap | Unlimited |
| `set_activation_concurrency (...)` | How many cold activations run concurrently | Runtime default |
| `set_default_request_timeout (...)` | This node's call reply cap | The root value |
| `peer_connections ()` | Manual peer connections | Location-store auto-discovery |
| `configure_router_socket ()` | See §3.1 below | The table below |

### 3.1 Socket Caps

Fields of the `mesh_node_socket_config_t` that `configure_router_socket ()` returns.

| Field | What it sets | Default |
| --- | --- | --- |
| `max_message_size` | Max size of a single accepted message | 16 MiB |
| `send_high_water_mark` | Bytes kept queued per peer to send | 4,096,000 |
| `receive_high_water_mark` | Bytes kept queued per peer after receipt | 4,096,000 |
| `mailbox_message_budget` | Message count an owner's mailbox holds | 1024 |
| `mailbox_byte_budget` | Payload bytes an owner's mailbox holds | 64 MiB |
| `receive_timeout` · `send_timeout` | If set, the cap on waiting in that direction | None |

How the two high-water marks work and how to pick their values is covered by
[4. Backpressure](04-backpressure.en.md).
`0` is not the default — it means **unlimited.** Leave the value unset to let it
auto-calculate.

The two `mailbox_*` values are set **only before startup.** `0` doesn't mean unlimited —
it picks the finite default the Framework profile sets.

### 3.2 Host-Wide Receive Cap

The surface `configure_inbound_dispatch ()` returns. It differs in nature from the per-
connection cap — it applies to the **total payload** of messages that haven't yet started
handler execution.

| Option | What it sets | Default |
| --- | --- | --- |
| `set_application_hwm_bytes (...)` | The host-wide cap, in bytes | Auto-calculated by profile if unset |
| `set_application_hwm_profile (...)` | The tendency used for auto-calculation | `balanced` |
| `set_process_memory_limit_bytes (...)` | The memory baseline for auto-calculation | The container/cgroup cap |

Passing `0` to `set_process_memory_limit_bytes` is rejected at startup. Leave the value
unset to mean unlimited.

> This unit and cap are confirmed by contract but **the runtime doesn't use them yet.**
> See [4. Backpressure §6](04-backpressure.en.md#6-framework-runtime-coverage).

## 4. Diagnostics

The surface `configure_dispatch ()` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `message_flow (...)` | Recording level | `errors_only` |
| `trace_sample_rate (double)` | Sampling ratio | 1.0 |
| `include_message_sizes (bool)` | Whether to record payload byte size too | Not recorded |
| `trace_log_file (path)` | A file written separately from app logs | Not separated |
| `set_message_flow_observer (...)` | Receiving records in your program | None |

What's recorded per level, and how to use an observer, is covered in
[11. Monitoring](11-monitoring.en.md).

## 5. Location Options

Fields of the `location_options_t` that `configure_locations ()` returns.

| Field | What it sets | Default |
| --- | --- | --- |
| `owner_lease_renew_interval` | Owner lease renewal interval | 5 seconds |
| `owner_lease_ttl` | Lease validity period | 15 seconds |
| `owner_lease_renew_timeout` | The cap on a renewal call | 3 seconds |
| `owner_lease_fencing_margin` | Margin that excludes the previous owner | 5 seconds |
| `polling_interval` | Store query interval | 1 second |
| `store_failure_grace` | How long a store outage is tolerated | 30 seconds |
| `route_cache_max_age` | Route cache validity period | 15 seconds |
| `message_follow_duration` | How long a message keeps following a target that's relocating | 30 seconds |
| `max_active_outbound_relocations` | Concurrent outbound relocation count | 64 |
| `max_active_inbound_relocations` | Concurrent inbound relocation count | 64 |
| `max_concurrent_relocation_captures` | Concurrent capture count | 8 |
| `max_concurrent_relocation_restores` | Concurrent restore count | 8 |
| `max_relocation_payload_in_flight_bytes` | Total payload cap while relocating | 256 MiB |
| `spot_router_channels` | Mapping when the Spot mesh name differs from the route channel name | Uses the same name |

**Keep `owner_lease_ttl` comfortably larger than `owner_lease_renew_interval`.** The lease
has to survive one failed renewal so a brief store delay doesn't change owners. The
defaults are 5 seconds to 15 seconds — a factor of three.

## 6. STREAM Options

Specified on the builder that `add_stream_node (name)` returns.

| Option | What it sets | Default |
| --- | --- | --- |
| `bind (endpoint)` | The address clients connect to | Must be specified |
| `enable_actor_dispatch ()` | Lets a session relay to an Actor | Not enabled |
| `register_session<TSession> ()` | The session type created per connection | Must be specified |
| `set_tls_server (cert, key, require_client_cert)` | TLS configuration | Plaintext |

> **Call `enable_actor_dispatch ()` only once per STREAM node.** Calling it twice on the
> same node throws `request_protocol_error`.

Even on the same profile, the STREAM socket uses smaller caps than a MeshNode. See
[9. STREAM](09-stream.en.md).

## 7. What You Can Change While Running

Only **two weights** can change after startup. Everything else is set before startup.

| Value | Surface | What it's for |
| --- | --- | --- |
| Placement weight | `route_mesh_runtime_options_t::placement_weight (int)` | Remove or restore this node as a new-object placement target |
| Channel weight | `...channel (name).weight (int)` | Remove or restore this node as a new select-one target |

Setting either to `0` **only stops new assignments.** Existing objects and connections stay
alive as-is. Used in zero-downtime deployment: stop new traffic from going to this node,
then start relocation ([12. Operations](12-operations.en.md) §4).

## 8. What You Must Set

These have no default, so startup fails if you don't specify them.

| Value | Where |
| --- | --- |
| A MeshNode's `listen` address | `add_route_mesh (...).listen (...)` |
| A STREAM node's `bind` address and session type | `add_stream_node (...)` |
| A fanout publisher's endpoint | `add_fanout_channel (...).enable_publisher (...)` |
| The Object role of a node that places Spots/Actors | `set_object_role (object_role_t::server)` |
| The location store, when using multiple nodes | `add_location_store (...)` |

## 9. Common Problems

- **I left it at `0` and memory keeps growing** → `0` on a high-water mark isn't the
  default — it means unlimited. Leave the value unset to let it auto-calculate.
- **I set timeout to 0 and startup fails** → that's expected.
  `set_default_request_timeout` rejects anything at or below 0.
- **I set `process_memory_limit_bytes` to 0 and it's rejected** → leave the value unset to
  mean unlimited. `0` is an invalid value.
- **The lease keeps getting taken away** → `owner_lease_ttl` is too short relative to
  `owner_lease_renew_interval`. Leave margin to survive one failed renewal.
- **I set weight to 0 and thought it dropped existing connections** → weight blocks **only
  new assignments.** Existing objects and connections stay up.
- **I tried to change a socket cap while running** → socket settings are only set before
  startup. The only values changeable while running are the two in §7.

## 10. Related Documents

- The formal contract: [C++ configuration and host public contract](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.en.md)
- Reading values from a config file: [19. Configuration](19-configuration.ko.md)
- What each cap changes: [4. Backpressure](04-backpressure.en.md)
- The procedure for draining traffic with weights: [12. Operations](12-operations.en.md)
