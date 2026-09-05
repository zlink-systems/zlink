import socket as _socket
import threading
import time

import zlink


_ALLOCATED_TCP_PORTS = set()


def tcp_endpoint():
    while True:
        sock = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
        sock.close()
        if port not in _ALLOCATED_TCP_PORTS:
            _ALLOCATED_TCP_PORTS.add(port)
            return port, f"tcp://127.0.0.1:{port}"


def _poll_monitor_event(monitor, timeout_ms):
    with zlink.create_poller() as poller:
        poller.add_monitor(monitor, zlink.PollEventFlag.POLLIN, 0)
        events = zlink.create_poll_events(1)
        try:
            if poller.wait(events, timeout_ms):
                return monitor.recv(flags=zlink.RecvFlags.DONT_WAIT)
            return None
        finally:
            poller.remove_monitor(monitor)


def wait_connected(*monitors, timeout_ms=5000):
    pending = list(monitors)
    deadline = time.monotonic() + (timeout_ms / 1000.0)

    while pending:
        remaining_ms = int((deadline - time.monotonic()) * 1000)
        if remaining_ms <= 0:
            raise TimeoutError("connection handshake did not complete")

        next_pending = []
        for monitor in pending:
            event = _poll_monitor_event(monitor, remaining_ms)
            if event is None:
                next_pending.append(monitor)
                continue
            if not (int(event.event) & int(zlink.MonitorEventMask.CONNECTION_READY)):
                next_pending.append(monitor)
        pending = next_pending


def wait_socket_monitor_event(monitor, expected_event, timeout_ms=5000):
    deadline = time.monotonic() + (timeout_ms / 1000.0)
    expected_value = int(expected_event)

    while True:
        remaining_ms = int((deadline - time.monotonic()) * 1000)
        if remaining_ms <= 0:
            raise TimeoutError("socket monitor did not produce the expected event")
        event = _poll_monitor_event(monitor, remaining_ms)
        if event is None:
            continue
        if int(event.event) & expected_value:
            return event


def wait_until(predicate, timeout_ms=5000, description="condition"):
    deadline = time.monotonic() + (timeout_ms / 1000.0)
    waiter = threading.Event()
    while time.monotonic() < deadline:
        if predicate():
            return
        waiter.wait(0.001)
    raise TimeoutError(f"timed out waiting for {description}")


def submit_request_op(op, *, timeout=2, description="request"):
    try:
        messages = op.timeout(timeout).submit_sync()
    except zlink.RequestError as error:
        raise RuntimeError(f"{description} failed: {error.result!r}") from error
    try:
        return [message.to_bytes() for message in messages]
    finally:
        for message in messages:
            message.close()
