
# Monitoring raw sockets

Socket monitors expose transport and protocol events without changing the data
receive path. Open a monitor with `zlink_socket_monitor_open()` and consume it
through the pull API.

## Receive mode

Call `zlink_socket_monitor_recv()` directly or register the monitor with a
poller. This mode fits an event loop that already owns scheduling.

## Poller-driven pull

Register the monitor with a poller when one event loop owns scheduling. After
readiness, call `zlink_socket_monitor_recv()` to drain the event; Core exposes
no monitor callback registration path.

## Snapshot

`zlink_monitor_status()` returns the current raw socket state and diagnostic
counters, including pending-message and automatic-HWM fields. A snapshot is a
point-in-time observation; use the event stream when transition ordering
matters.

## Close

Close the monitor with `zlink_monitor_close()`. Closing a socket also terminates
its monitor source. Code that polls both handles must accept termination on
either path.

Monitor events do not contain application payload or topology state.
