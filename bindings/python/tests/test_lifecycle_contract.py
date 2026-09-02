"""Regression tests for retryable native ownership transitions."""

import errno
from unittest.mock import Mock, patch

import pytest

import zlink
from zlink._runtime.core import context as context_runtime
from zlink._runtime.eventing import monitor as monitor_runtime
from zlink._runtime.eventing import poller as poller_runtime
from zlink._runtime.eventing import timer as timer_runtime
from zlink._runtime.handles import native_support
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


def _socket_with_handle(socket_cls, handle):
    owner = object.__new__(socket_cls)
    owner._socket_handle = handle
    return owner


def test_socket_handle_preserves_native_handle_after_busy_for_retry():
    native = _NativeCloseStub("zlink_close", [zlink.CloseResult.BUSY, 0])
    handle = socket_base._SocketHandle(123, own=True)
    with patch.object(socket_base, "lib", return_value=native):
        with pytest.raises(zlink.CloseError):
            handle.close()
        assert handle.handle == 123
        handle.close()
    assert handle.handle is None
    assert len(native.calls) == 2


def test_socket_close_preserves_handle_for_retry():
    handle = _RetryableHandle()
    socket = _socket_with_handle(socket_base._BaseSocket, handle)
    with pytest.raises(zlink.CloseError):
        socket.close()
    assert handle.calls == 1
    socket.close()
    assert handle.calls == 2


def test_socket_resource_exit_retries_native_busy_until_close_succeeds():
    handle = _RetryableHandle()
    socket = _socket_with_handle(socket_base._BaseSocket, handle)
    socket.__exit__(None, None, None)
    assert handle.calls == 2


def test_socket_resource_exit_does_not_retry_non_busy_close_error():
    handle = Mock()
    handle.close.side_effect = zlink.CloseError(
        zlink.CloseResult.INVALID_HANDLE, errno.EFAULT
    )
    socket = _socket_with_handle(socket_base._BaseSocket, handle)
    with pytest.raises(zlink.CloseError):
        socket.__exit__(None, None, None)
    handle.close.assert_called_once_with()


def test_completion_owner_is_shutdown_before_each_native_close_attempt():
    handle = _RetryableHandle()
    socket = _socket_with_handle(socket_base_impl.DealerSocket, handle)
    completion = Mock()
    socket._completion_owner = completion
    with pytest.raises(zlink.CloseError):
        socket.close()
    completion.shutdown.assert_called_once_with()
    socket.close()
    assert completion.shutdown.call_count == 2


@pytest.mark.parametrize(
    ("runtime_module", "runtime_cls", "method_name"),
    [
        (monitor_runtime, monitor_runtime.NativeMonitorSocket, "zlink_monitor_close"),
        (timer_runtime, timer_runtime.NativeTimer, "zlink_timer_destroy"),
    ],
)
def test_pull_event_owner_preserves_handle_after_failed_destroy(
    runtime_module, runtime_cls, method_name
):
    native = _NativeCloseStub(method_name, [zlink.CloseResult.BUSY, 0])
    owner = object.__new__(runtime_cls)
    owner._handle = 123
    with patch.object(runtime_module, "lib", return_value=native):
        with pytest.raises(zlink.CloseError):
            owner.close()
        assert owner._handle == 123
        owner.close()
    assert owner._handle is None


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
