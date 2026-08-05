[한국어](06-monitoring.ko.md)

# Monitoring raw sockets

Socket monitors expose transport and protocol events without changing the data
receive path. Open a monitor with `zlink_socket_monitor_open()` and choose one
consumption mode.

## Receive mode

Call `zlink_socket_monitor_recv()` directly or register the monitor with a
poller. This mode fits an event loop that already owns scheduling.

## Callback mode

Install `zlink_socket_monitor_handler()`. The callback runs on the Core control
runtime, not on the monitored socket's I/O thread. Keep the callback short so
later monitor and timer callbacks are not delayed.

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
