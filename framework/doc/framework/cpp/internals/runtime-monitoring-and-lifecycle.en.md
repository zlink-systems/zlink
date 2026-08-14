# C++ Runtime Monitoring and Location Lifecycle

This document describes how the C++ framework combines the public
`route_mesh_runtime_t` model with the Location Store. The Application status model
and the binding monitor-event model are different semantic models, so the C++ runtime
translates between them.

## 1. Responsibility boundary

`route_mesh_runtime_service_t` projects transport state, Location Store observations,
and application-claim state into one `mesh_node_snapshot_t`. Public status exposes
only the following information:

- MeshNode state and `is_ready`
- peer Routing ID and peer state
- ready target count for each Channel
- placement availability and active Actor/Spot counts
- a monotonically increasing snapshot `sequence`

Endpoint, descriptor revision, owner lease, connection generation, and native monitor
event DTOs are not part of the public snapshot. This prevents binding types from
leaking into the Framework domain model and keeps transport identity separate from
service readiness.

## 2. Snapshot and observer ordering

When transport topology, Location descriptor polling, or host lifecycle changes, the
runtime service:

1. builds a snapshot from the current node and Location Store state;
2. increments the MeshName-specific `sequence`;
3. stores the latest snapshot in the hub;
4. enqueues the snapshot in each observer's bounded queue; and
5. invokes the observer callback from the queue's execution owner.

An observer exception does not escape to another observer, transport lifecycle, or
application dispatch. A slow observer does not block the runtime. When its queue is
pressured, it catches up with the latest snapshot, so the callback does not rebuild a
snapshot or interpret binding monitor events directly.

## 3. Structured peer events

`zlink.runtime.mesh_node.peer_changed` is derived from peer-state changes in public
snapshots. The C++ RuntimeMonitoring E2E service compares the public `observe()` result:
it records `ConnectionReady` when a peer enters the ready set and `Disconnected` when
it leaves. Each record includes the MeshName, Routing ID, snapshot sequence, and current
topology state.

The record does not expose an internal descriptor generation. Crash replacement is
verified by confirming that the old peer leaves the ready list after the owner lease
expires, the replacement with the same RID becomes ready, and the request immediately
after readiness is processed exactly once.

## 4. Physical connection identity from the binding

The binding public `monitor_event_t` preserves the native Core fields
`connection_id`, `transport_pair_id`, `transport_pair_generation`, `transport_lane`,
and `flags`. The Framework mesh owner uses `connection_id`, not `value`, when it finds
the readiness or disconnect target.

`value` is an event-specific payload and is not the identity of a physical transport
attempt. Confusing the two can make an old connection remove a new one or classify a
valid replacement as stale. The conversion uses only public binding fields and does
not access native private storage or reflection.

## 5. Crash replacement and owner leases

Normal shutdown can remove the descriptor and lease during owner cleanup, but a forced
termination cannot. A replacement claiming the same role and RID therefore receives
`rejected_conflict` while the old owner lease remains valid.

The C++ RuntimeMonitoring E2E runner does not start the replacement as soon as the peer
becomes not-ready. It waits for the configured owner-lease TTL and fencing margin, then
starts the replacement. A takeover bypass before expiry is not used because a stale
owner could overwrite the current descriptor.

## 6. Logging-provider isolation

A Framework logging callback is an observation boundary. If a callback sink throws,
the logger completes that sink invocation and continues dispatch and host lifecycle.
Sinks are invoked independently, so one sink failure does not suppress another sink.
The same rule applies to the throwing monitoring E2E profile.

## 7. Verification locations

- binding monitor contract: `bindings/cpp/tests/contract/test_cpp_contract_monitor.cpp`
- C++ RuntimeMonitoring E2E: `framework/languages/cpp/e2e/RuntimeMonitoring/run_e2e.sh`
- public snapshot contract: `framework/languages/cpp/framework/include/zlink/framework/contracts/monitoring/route_mesh_runtime.hpp`
- runtime projection: `framework/languages/cpp/framework/src/runtime/mesh/route_mesh_runtime_service.cpp`
- monitor identity conversion: `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp`

## 8. Store failure and snapshot reads

`route_mesh_runtime_service_t::snapshot()` does not query the Location Store directly. The runtime
pump performs the store query and updates a descriptor cache for each mesh. A snapshot combines that
cache with native topology, so a store timeout cannot block a monitoring HTTP call or an observer's
current snapshot read.

When the Location runtime heartbeat detects a stale owner token, it reclaims the lease using the same
owner identity. On success it records the new token and Store time and restores `store_healthy`.
When a restarted process reuses a routing identity, it publishes its descriptor only after the
previous owner's TTL and fencing margin have elapsed.

## 9. Monitor event ABI and callback lifetime

The extended monitor identity fields are read through the current 0.11.1
`zlink_socket_monitor_recv` entry point. Core exposes no separate receive entry
point for the previous event prefix and no size/version negotiation. The C++
binding uses the current event layout for pull-based monitoring and callback
dispatch, and passes only public binding fields to the Framework.

The C++ `socket_monitor_t` callback userdata points to a heap-owned callback
state held by the monitor implementation, rather than to the movable
`socket_monitor_t` object. Moving a monitor therefore does not leave Core with
the address of a moved-from or destroyed object. Callback exceptions are
contained at the Core callback boundary; callback-depth accounting and
self-close cleanup still run after an exception.

## 10. Descriptor-cache initialization and an absent Store query

The runtime service primes its descriptor cache before starting the pump. An immediate `snapshot()`
therefore includes placement and local descriptor data without waiting for the first polling interval or
reading the Store on every snapshot call.

An in-process projection without a `location_runtime_query_t` uses caller-supplied descriptors as its
complete input. The absence of a health cache does not downgrade that snapshot to `degraded`. When a query
service is connected, the projection uses the cached Store health and reports Store failure as `degraded`.

When Store descriptors are authoritative for the topology, a peer missing from the Store is not retained as
a public ready candidate merely because an old transport-topology entry remains. This prevents public
readiness from reusing a descriptor that the Store has already removed.
