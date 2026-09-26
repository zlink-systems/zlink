"""Core decides a poller wait and its serialization (05-polling §2, §5, §7).

The binding enters Core for every call and reports Core's configuration
result; it adds no wait of its own.
"""

import errno
from unittest.mock import patch

import pytest

import zlink
from zlink._runtime.eventing import poller as poller_runtime
from zlink._runtime.messaging.routed_async import _DrainResult

ETERM = int(zlink.ErrorCode.ETERM)


def test_failed_wait_reports_the_core_configuration_result():
    """After a context shutdown Core ends the wait of a poller with a socket
    of that context with ZLINK_CONFIG_INTERNAL_ERROR / ETERM."""
    context = zlink.create_context()
    socket = zlink.create_pair_socket(context)
    poller = zlink.create_poller()
    try:
        poller.add_socket(socket, zlink.PollEventFlag.POLLIN, 1)
        context.shutdown()
        with pytest.raises(zlink.ConfigError) as raised:
            poller.wait(zlink.create_poll_events(1), 0)
        assert raised.value.result == zlink.ConfigResult.INTERNAL_ERROR
        assert raised.value.native_errno == ETERM
    finally:
        poller.close()
        socket.close()
        context.close()


class _WritableOnlyNative:
    """Reports one completion event whose drain delivers no REQUEST record."""

    def __init__(self, real, socket_key):
        self._real = real
        self._socket_key = socket_key
        self.waits = 0

    def __getattr__(self, name):
        if name != "zlink_poller_wait":
            return getattr(self._real, name)

        def wait(handle, events, capacity, timeout_ms, error_out):
            self.waits += 1
            events[0].source_kind = int(zlink.PollSourceKind.SOCKET)
            events[0].socket = self._socket_key
            events[0].events = int(zlink.PollEventFlag.POLLCOMPLETION)
            return 1

        return wait


class _WritableOnlyOwner:
    def has_managed_writable_wait(self):
        return True

    def drain(self, poller):
        return _DrainResult(total_count=1, request_count=0)


def test_wait_returns_once_when_only_writable_progress_was_filtered():
    """A WRITABLE record that only advanced a managed retry is not a caller
    event: the wait returns 0 without waiting again, as in C++, Go and Node."""
    context = zlink.create_context()
    poller = zlink.create_poller()
    socket_key = 0x1000
    poller._socket_registrations[socket_key] = [
        object(),
        int(zlink.PollEventFlag.POLLCOMPLETION),
        _WritableOnlyOwner(),
    ]
    native = _WritableOnlyNative(poller_runtime.lib(), socket_key)
    try:
        with patch.object(poller_runtime, "lib", lambda: native):
            ready = poller.wait(zlink.create_poll_events(1), 2000)
        assert ready == 0
        assert native.waits == 1
    finally:
        poller._socket_registrations.clear()
        poller.close()
        context.close()


def test_monitor_open_failure_is_a_configuration_error():
    """A monitor open that Core refuses is a ConfigError with Core's
    configuration projection of its errno, as in C++ and Go."""
    context = zlink.create_context()
    socket = zlink.create_pair_socket(context)
    monitor = socket.monitor_open()
    try:
        with pytest.raises(zlink.ConfigError) as raised:
            socket.monitor_open()
        assert raised.value.result == zlink.ConfigResult.INVALID_STATE
        assert raised.value.native_errno == errno.EBUSY
    finally:
        monitor.close()
        socket.close()
        context.close()
