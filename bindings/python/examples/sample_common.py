"""Common helpers for zlink Python samples."""

import socket as _socket

import zlink


def tcp_endpoint():
    """Allocate an ephemeral TCP port and return a tcp://127.0.0.1:<port> endpoint."""
    sock = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port, f"tcp://127.0.0.1:{port}"


def wait_connected(*monitors, timeout_ms=5000):
    """Wait for CONNECTION_READY on all monitors.

    Each monitor should be opened with MonitorEventMask.CONNECTION_READY.
    Blocks until every monitor reports the event or timeout expires.
    """
    import time

    event_mask = int(zlink.MonitorEventMask.CONNECTION_READY)
    remaining = set(range(len(monitors)))
    deadline = time.monotonic() + timeout_ms / 1000.0

    with zlink.create_poller() as poller:
        events = zlink.create_poll_events(max(1, len(monitors)))
        for i, monitor in enumerate(monitors):
            poller.add_monitor(monitor, zlink.PollEventFlag.POLLIN, i)
        while remaining:
            remaining_ms = int((deadline - time.monotonic()) * 1000)
            if remaining_ms <= 0 or poller.wait(events, remaining_ms) == 0:
                raise TimeoutError(
                    f"wait_connected: {len(remaining)} monitor(s) did not report ready"
                )
            for index in range(events.ready_count):
                i = events.slot(index)
                while (event := monitors[i].recv(flags=zlink.RecvFlags.DONT_WAIT)) is not None:
                    if int(event.event) & event_mask:
                        remaining.discard(i)
        for monitor in monitors:
            poller.remove_monitor(monitor)
