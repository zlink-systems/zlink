[한국어](socket-option-defaults.ko.md)

# Socket option defaults

`options_t` stores common raw-socket and transport defaults. Typed socket
implementations validate pattern-specific options before applying them.

## Queue planning

`sndhwm` and `rcvhwm` are 64-bit accounted-byte limits. Their manual default is
`4,096,000` bytes, and `0` means unlimited. There is no message-count HWM
compatibility state. A runtime shrink keeps already queued messages and defers
the effective reduction until retained bytes fall below the new limit.

Automatic HWM selects a policy from the raw socket role, profile, planning
unit, and observed connection count. The selected slot value is multiplied by
the 64-bit planning unit to produce the final byte HWM. The slot and effective
message-size fields remain diagnostics; pipe admission always uses retained
bytes. Hysteresis prevents rapid bucket changes near a connection-count
boundary.

The Core pipe low watermark is `ceil(hwm_bytes / 2)`. This value controls byte
credit updates and is not configurable through a Framework receive-resume
profile.

## Application-visible state

`zlink_monitor_status()` ABI version 2 exposes planned, applied, and deferred
64-bit HWM byte values; deferred-value validity; bytes in flight; the minimum
message charge; and oversize single-message admission counters. These fields
are diagnostic snapshots. Applications configure policy inputs through public
options rather than mutating internal values.

## Transport defaults

Reconnect, TCP keepalive, kernel buffers, TOS, handshake intervals, and TLS
fields are applied by the relevant transport. Unsupported combinations fail
through the typed configuration result.
