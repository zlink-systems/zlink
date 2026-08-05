# 07. Location authority

[Reference index](README.en.md)

This category covers Location/Relocation Store registration, `location_options_t` tuning, and the
entry points `location_readiness_t` and `location_runtime_query_t` provide. The exact signatures
are owned by the
[Location/Relocation Store/Redis exact interface](../../common/spec/server/languages/cpp/interfaces/07-location-store.en.md)
(Korean-only).

---

## Location/Relocation Store registration (configuration time)

A host that uses distributed discovery, Instance Spot cold activation, or Actor/Spot relocation
registers a Store implementation at the root.

```cpp
options.add_location_store(
  std::make_shared<zlink::framework::redis::redis_location_store_t>(
    zlink::framework::redis::redis_location_options_t{
        .connection_string = "redis-host:6379",
        .key_prefix = "zlink:game:location",
    })); // registers a provider that stores small opaque location records

options.add_relocation_store(
  std::make_shared<zlink::framework::redis::redis_relocation_store_t>(
    zlink::framework::redis::redis_relocation_options_t{
        .connection_string = "redis-host:6379",
        .key_prefix = "zlink:game:relocation",
    })); // registers immutable relocation payloads as a separate capability
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.add_location_store(shared_ptr<location_store_t>)` | Without it, distributed discovery/relocation is unavailable | One Store providing exact read, conditional atomic write (`write`), and bounded prefix scan (`scan`) |
| `.add_relocation_store(shared_ptr<relocation_store_t>)` | Required if any factory uses `preserve_state_with`/`recreate_on_relocation`, or any Instance Spot factory exists | One Store that stores immutable relocation payloads under references the Framework issues |
| `redis_location_options_t::key_prefix` / `redis_relocation_options_t::key_prefix` | The code default is an empty string, but a valid configuration must specify a non-empty value (and the two must differ if they share the same Redis) | The Redis key namespace |
| `.connection_string` | Required | The Redis connection setting |
| `.operation_timeout` | 5 seconds | The provider I/O upper bound |

**Completion result.** Registers synchronously with no return value. Each role registers exactly
one — registering the same role twice, or missing a required Store, surfaces as a configuration
error in `app.run(...)`'s startup validation.

**When to use.** A node that only uses manual peers and needs no distributed location feature can
start by omitting this entry. Besides the official Redis provider, another provider implementing
the same `location_store_t`/`relocation_store_t` (linking only the opt-in CMake target
`zlink::framework_provider_abstractions`) can also be registered. After registration, the
application does not call Store operations directly, nor swap or release the Store.

---

## `configure_locations()` (configuration time)

Tunes owner lease, polling, and the relocation concurrency cap.

```cpp
zlink::framework::location_options_t &locations = options.configure_locations();
locations.owner_lease_ttl = std::chrono::seconds{20};
locations.max_concurrent_relocation_captures = 16;
```

**Options.** Commonly tuned values are as follows.

| Field | Default | Meaning |
| --- | --- | --- |
| `owner_lease_renew_interval` / `owner_lease_ttl` / `owner_lease_fencing_margin` / `owner_lease_renew_timeout` | 5s / 15s / 5s / 3s | The owner lease's renewal interval and TTL. Must satisfy `renew_interval + renew_timeout < ttl - fencing_margin` |
| `polling_interval` | 1 second | The Store status-check interval |
| `store_failure_grace` | 30 seconds | The grace period tolerating a Store failure |
| `route_cache_max_age` / `message_follow_duration` | 15s / 30s | `0` disables the feature. If both are positive, cache age must be at least 5 seconds smaller than message follow duration |
| `max_active_outbound_relocations` / `max_active_inbound_relocations` | 64 / 64 | The cap on concurrently in-progress relocation units |
| `max_concurrent_relocation_captures` / `max_concurrent_relocation_restores` | 8 / 8 | The cap on concurrently executable Capture/Restore callbacks |
| `max_relocation_payload_in_flight_bytes` | 268,435,456 | The process-wide cap on encoded relocation payload in-flight |

**Completion result.** A synchronous setting. If the lease/polling values are 0 or below, or
violate the inequality above, it surfaces in startup validation. A value change while running
applies only to new relocation admissions.

**When to use.** Adjust this only when the defaults do not fit the deployment environment
(network latency, Store response time).

---

## `is_peer_ready` (location_readiness_t)

Checks whether a peer of a specific MeshName/role (optionally a specific node) is ready.

```cpp
bool ready = co_await location_readiness.is_peer_ready(
  "play", zlink::framework::location_role_t::spot);
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `node_rid` | `std::nullopt` (based on the entire role) | Narrows the check to a specific node |

**Completion result.** Returns `bool`. Reports only readiness, with no separate failure kind.

**When to use.** Use this for startup ordering control or a health check that waits until a peer
of a specific role is ready.

---

## `get_status` (location_runtime_query_t)

Checks the Location runtime's own status (Store connection, owner lease renewal).

```cpp
zlink::framework::location_runtime_status_t status =
  co_await location_query.get_status();
bool healthy = status.store_healthy && status.owner_lease_healthy;
```

**Options.** This entry point has no modifiers.

**Completion result.** Returns `location_runtime_status_t`. `store_healthy` and
`owner_lease_healthy` respectively indicate Store connection and owner lease renewal status, and
`last_refresh_at`/`owner_lease_renewed_at` give the last refresh time.

**When to use.** Use this to diagnose the health of the Location infrastructure itself. Use
`is_peer_ready` to check whether a specific peer is ready.

---

## `list_topology` / `list_service_summaries` (location_runtime_query_t)

Queries registered node topology or per-MeshName service summaries, page by page.

```cpp
zlink::framework::location_page_t<zlink::framework::location_topology_entry_t> page =
  co_await location_query.list_topology(
    zlink::framework::location_topology_filter_t{
        .mesh_name = "play",
        .state = zlink::framework::location_topology_state_t::ready,
    },
    zlink::framework::location_page_request_t{.page_size = 200});
```

**Options.** Both calls take the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| filter (`location_topology_filter_t`/`location_service_summary_filter_t`) | Everything (every field `std::nullopt`) | Narrows results by MeshName/NodeRid/State |
| `page.page_size` | 100 | Range 1..1000 |
| `page.continuation_token` | `std::nullopt` (first page) | The opaque token the previous response returned. The application does not interpret it directly or reuse it in a different query |

**Completion result.** Returns `location_page_t<T>`. `std::nullopt` for `continuation_token` means
the last page. Internal information such as Store key/version, owner lease generation, or the
descriptor payload is not returned.

**When to use.** Use this in an operational tool to query registered nodes or service status in a
human-readable form. Use the status-query entry in the topology-discovery category for
real-time availability of a single MeshName/ChannelName.

---

See the
[Location/Relocation Store/Redis exact interface](../../common/spec/server/languages/cpp/interfaces/07-location-store.en.md)
(Korean-only) for the full rationale.
