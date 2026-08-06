# Actor Placement and Destroy Cleanup Boundaries

This document describes the C++ Framework implementation boundaries for Actor placement and
destroy. The common Framework specification and the C++ public contract remain authoritative for
caller-visible behavior.

## Destroy ordering

Recreating the same `ActorId` requires the previous incarnation's authority, reserved capacity,
creation reservation, and process-local Actor state to be cleaned up together. Deleting authority
first can leave a creation reservation behind and make the next creation report exhausted capacity.
The local Actor state is therefore cleaned first, while the provider removes the authority,
decrements target capacity, and deletes the creation reservation in one conditional store write.

The provider retries only after reading the current authority and target descriptor again. If a
stale target descriptor fails the owner-lease check, the current descriptor is refreshed once and
the current lifecycle generation and lease token are used for the next decision.

## Callback lifetime and lock boundaries

Spot runtime holds its node mutex only while validating ownership and removing local Actor state.
The Mesh host cleanup callback runs after that mutex is released because the callback can re-enter
Spot runtime or the Location Store.

The Mesh host protects the destroy callback with a shared gate. Entry first checks the stopping
state and increments an active count; host stop prevents new entry and waits for active callbacks
to return. The callback accesses the host only after entering the gate, so a callback retained by a
Spot runtime cannot use a destroyed host.

## E2E fixture boundary

TA-B2 verifies the difference between ID-only messaging and an exact `ActorRef` lifecycle
operation. The C++ fixture uses the public `actor_manager_t` and `actor_client_t` APIs and expects
the previous `ActorRef` operation to end in `invalid_operation`.

TA-B3 requires a network block between the caller and the current Actor owner. C++
`mesh_peer_connections_t` is a semantic Port: its connect and disconnect operations update the
intent list and invoke the running Mesh runtime with the expected routing ID. The fixture does not
inspect an internal registry. The runner scopes transport observation to sockets owned by the caller
process so connections from other peers using the actor-b endpoint are not counted as the caller
route, and it uses the public peer snapshot from `route_mesh_runtime_t` for route readiness.

When an explicit disconnect targets an admitted peer, the runtime captures that peer's
`connection_id`, closes the endpoint, and removes the same connection from both the topology and
liveness registries. Closing only the socket leaves a stale peer available for admission and can
produce `request_failed`, so these state changes form one disconnect boundary.

An explicit disconnect can also arrive after monitor or liveness processing has already removed
the topology entry. In that case the runtime finds every physical connection still held in the
candidate registry for the endpoint and removes the candidate, topology entry, and liveness entry
using each candidate's routing ID and `connection_id`. Disconnecting the endpoint therefore does
not leave an old physical connection eligible for admission after a reconnect. The cleanup is
idempotent and applies both when an admitted peer was found and when the no-admitted path is used.

Mesh liveness is evaluated in the common maintenance phase of
`public_host_runtime_t::dispatch_ready()`. A timed-out peer is removed from the topology and the
public snapshot reports it as `not_connected`. If request admission still ends with a deadline
terminal after the snapshot becomes `not_ready`, the common-contract `unavailable` terminal is
still missing and TA-B3 must not be considered complete.

Actor request wire terminals are converted to Framework public error kinds by
`request_failure_mapper_t::reply_header_exception()`. A non-zero transport terminal is also
converted to `unavailable` when the target peer is no longer admitted and serving. If the peer is
still serving, the original application or timeout meaning is preserved. Callers and E2E fixtures
do not read `detail` state for this conversion; an unusable route is verified as the common-contract
`unavailable` result.

Remote Actor send/request admission does not accept a request merely because the topology contains
the peer. The location state and lifecycle-generation comparison used by
`route_mesh_runtime_service_t` for the public peer snapshot is shared through an internal
readiness resolver. If the topology peer is gone, its Location descriptor is not serving, or its
generation does not match, the operation is rejected with `not_connected` before it reaches the
remote handler. The Actor client maps this result to `unavailable`; it is distinct from changing a
request to failure after the handler has already run and a later public snapshot is read.

The current TA-B3 implementation verifies the disconnect-side `Unavailable` result, public
`not_ready` propagation, bidirectional peer reconnect readiness, and the restored request reply.
The runner applies an HTTP timeout to route-state polling so a stalled transport or HTTP endpoint
cannot leave cleanup waiting indefinitely.

The Actor client preserves `kind()` when it receives a `framework_exception_t`. A Framework
error therefore cannot be changed into another public error merely because its message contains
words such as `stale` or `not found`. A native `std::exception` without a typed kind is limited
to the legacy binding-transport path; only that path interprets known transport error text for
compatibility.
