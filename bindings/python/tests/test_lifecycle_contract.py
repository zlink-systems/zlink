"""Regression tests for retryable native ownership transitions."""

import errno
import threading
from types import SimpleNamespace
from unittest.mock import Mock, patch

import pytest

import zlink
from zlink._runtime.core import context as context_runtime
from zlink._runtime.eventing import monitor as monitor_runtime
from zlink._runtime.eventing import poller as poller_runtime
from zlink._runtime.eventing import timer as timer_runtime
from zlink._runtime.handles import native_support
from zlink._runtime.messaging.request_reply import _PendingRequest
from zlink._runtime.sockets import socket_base, socket_base_impl


class _NativeCloseStub:
    def __init__(self, method_name, results):
        self._method_name = method_name
        self._results = list(results)
        self.calls = []

    def __getattr__(self, name):
        if name == self._method_name:
            return self._close
        if name == "zlink_errno":
            return lambda: errno.EBUSY
        raise AttributeError(name)

    def _close(self, handle):
        self.calls.append(handle)
        return self._results.pop(0)


class _RetryableHandle:
    def __init__(self):
        self.calls = 0

    def close(self):
        self.calls += 1
        if self.calls == 1:
            raise zlink.CloseError(zlink.CloseResult.BUSY, errno.EBUSY)


def _socket_with_lifecycle_state(socket_cls, handle):
    owner = object.__new__(socket_cls)
    owner._socket_handle = handle
    owner._recv_handler = object()
    owner._recv_handler_cb = object()
    owner._send_ready_handler = object()
    owner._send_ready_handler_cb = object()
    owner._packet_handler = object()
    owner._packet_handler_cb = object()
    owner._dispatcher = Mock()
    owner._request_state_lock = threading.RLock()
    owner._request_closing = False
    return owner


def test_socket_handle_preserves_native_handle_after_ebusy_for_retry():
    native = _NativeCloseStub("zlink_close", [zlink.CloseResult.BUSY, 0])
    handle = socket_base._SocketHandle(123, own=True)

    with patch.object(socket_base, "lib", return_value=native):
        with pytest.raises(zlink.CloseError) as raised:
            handle.close()
        assert raised.value.result == zlink.CloseResult.BUSY
        assert handle.handle == 123

        handle.close()

    assert handle.handle is None
    assert len(native.calls) == 2


def test_socket_close_keeps_callback_state_when_native_close_fails():
    handle = _RetryableHandle()
    socket = _socket_with_lifecycle_state(socket_base._BaseSocket, handle)
    callback_refs = (
        socket._recv_handler,
        socket._recv_handler_cb,
        socket._send_ready_handler,
        socket._send_ready_handler_cb,
        socket._packet_handler,
        socket._packet_handler_cb,
    )

    with pytest.raises(zlink.CloseError):
        socket.close()

    assert (
        socket._recv_handler,
        socket._recv_handler_cb,
        socket._send_ready_handler,
        socket._send_ready_handler_cb,
        socket._packet_handler,
        socket._packet_handler_cb,
    ) == callback_refs
    socket._dispatcher.close.assert_not_called()

    socket.close()

    assert all(
        value is None
        for value in (
            socket._recv_handler,
            socket._recv_handler_cb,
            socket._send_ready_handler,
            socket._send_ready_handler_cb,
            socket._packet_handler,
            socket._packet_handler_cb,
        )
    )
    socket._dispatcher.close.assert_called_once_with()


def test_request_callback_is_not_cancelled_until_socket_close_succeeds():
    handle = _RetryableHandle()
    socket = _socket_with_lifecycle_state(socket_base_impl.DealerSocket, handle)
    callback = Mock()
    socket._pending_requests = {1: _PendingRequest(callback=callback)}

    with pytest.raises(zlink.CloseError):
        socket.close()

    callback.assert_not_called()
    assert 1 in socket._pending_requests

    socket.close()

    callback.assert_called_once_with(zlink.RequestResult.TERMINATED, [])
    assert socket._pending_requests == {}


@pytest.mark.parametrize(
    ("runtime_module", "runtime_cls", "method_name"),
    [
        (monitor_runtime, monitor_runtime.NativeMonitorSocket, "zlink_monitor_close"),
        (timer_runtime, timer_runtime.NativeTimer, "zlink_timer_destroy"),
    ],
)
def test_event_owner_preserves_state_after_failed_destroy(
    runtime_module, runtime_cls, method_name
):
    native = _NativeCloseStub(method_name, [zlink.CloseResult.BUSY, 0])
    owner = object.__new__(runtime_cls)
    owner._handle = 123
    owner._handler = object()
    owner._handler_cb = object()
    if runtime_cls is monitor_runtime.NativeMonitorSocket:
        owner._dispatcher = Mock()

    with patch.object(runtime_module, "lib", return_value=native):
        with pytest.raises(zlink.CloseError):
            owner.close()
        assert owner._handle == 123
        assert owner._handler is not None
        assert owner._handler_cb is not None

        owner.close()

    assert owner._handle is None
    assert owner._handler is None
    assert owner._handler_cb is None
    if runtime_cls is monitor_runtime.NativeMonitorSocket:
        owner._dispatcher.close.assert_called_once_with()


def test_context_preserves_handle_after_failed_termination():
    native = _NativeCloseStub("zlink_ctx_term", [zlink.CloseResult.BUSY, 0])
    owner = object.__new__(context_runtime.NativeContext)
    owner._handle = 123

    with patch.object(context_runtime, "lib", return_value=native):
        with pytest.raises(zlink.CloseError):
            owner.close()
        assert owner._handle == 123
        owner.close()

    assert owner._handle is None


def test_poller_preserves_handle_after_failed_destroy():
    native = _NativeCloseStub("zlink_poller_destroy", [zlink.CloseResult.BUSY, 0])
    owner = object.__new__(poller_runtime.NativePoller)
    owner._handle = 123

    with patch.object(poller_runtime, "lib", return_value=native):
        with pytest.raises(zlink.CloseError):
            owner.close()
        assert owner._handle == 123

        owner.close()

    assert owner._handle is None
    assert len(native.calls) == 2


@pytest.mark.parametrize("error_type", (TypeError, BufferError))
def test_invalid_native_bridge_payload_is_not_reported_as_backpressure(error_type):
    bridge = Mock()
    bridge.send_parts.side_effect = error_type("invalid payload")
    owner = object.__new__(socket_base._BaseSocket)
    owner._socket_handle = SimpleNamespace(handle=123)

    with patch.object(socket_base, "_native_extension", bridge):
        with pytest.raises(error_type):
            owner._send_payload_via_native_bridge(object(), 1)


def test_unknown_native_result_is_preserved_in_typed_error():
    with pytest.raises(zlink.RequestError) as raised:
        native_support._raise_result_error(
            zlink.RequestError,
            zlink.RequestResult,
            999,
            errno.EIO,
        )

    assert raised.value.result == 999
    assert raised.value.code == 999
