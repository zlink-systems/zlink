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

    while remaining and time.monotonic() < deadline:
        for i in list(remaining):
            event = monitors[i].recv(zlink.RecvFlags.DONT_WAIT)
            if event is not None and (event.event & event_mask):
                remaining.discard(i)
        if remaining:
            time.sleep(0.005)

    if remaining:
        raise TimeoutError(
            f"wait_connected: {len(remaining)} monitor(s) did not report ready"
        )
