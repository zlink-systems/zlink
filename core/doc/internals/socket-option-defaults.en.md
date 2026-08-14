[한국어](socket-option-defaults.en.md)

# Socket option defaults

`options_t` stores common raw-socket and transport defaults. Typed socket
implementations validate pattern-specific options before applying them.

## Queue planning

`sndhwm` and `rcvhwm` are 64-bit accounted-byte limits. Their manual default is
`4,096,000` bytes, and `0` means unlimited. There is no message-count HWM
compatibility state. A runtime shrink keeps already queued messages and defers
the effective reduction until retained bytes fall below the new limit, then
applies the deferred shrink immediately.

Automatic HWM uses the context Core memory budget, profile role bounds, and a
registry of unique physical directional queues. The registry records one
inproc ypipe once rather than once per endpoint and identifies it with a stable
queue ID and generation. After manual reservations, bounded water-filling
starts each physical queue at its role minimum and raises unsaturated queues to
their role maximum. Division remainders are granted one byte at a time in
stable queue-ID order.

Core does not add the values of two inproc endpoints. One finite-manual endpoint
sets the cap; two finite-manual endpoints use the smaller cap; an
unlimited-manual endpoint paired with an automatic endpoint uses the automatic
plan. Two unlimited endpoints remain unlimited for admission while reserving
the role maximum once for planning.

The DEALER/ROUTER completion progress lane carries only terminal replies and
error replies. It applies no automatic or manual HWM, LWM, inproc boost, role
bounds, or Core budget reservation. Disabling automatic HWM preserves the last
applied HWM on live pipes and excludes them from subsequent automatic planning.

The Core pipe low watermark is `ceil(hwm_bytes / 2)`. This value controls byte
credit updates and is not configurable through a Framework receive-resume
profile.

## Application-visible state

`zlink_monitor_status()` ABI version 3 exposes planned, applied, and deferred
64-bit HWM byte values; pending-message counts and pending bytes; bytes in
flight; the minimum message charge; and oversize single-message admission
counters. The context budget snapshot distinguishes physical-queue capacity,
provisional and committed queue bytes, application-held leases, and completion
and monitor queues. These fields are diagnostic snapshots. Applications
configure policy inputs through public options rather than mutating internal
values.

## Transport defaults

Reconnect, TCP keepalive, kernel buffers, TOS, handshake intervals, and TLS
fields are applied by the relevant transport. Unsupported combinations fail
through the typed configuration result.
