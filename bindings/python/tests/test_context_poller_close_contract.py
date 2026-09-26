"""Context and poller close behavior from async-execution-model section 4."""

import errno
from unittest.mock import patch

import pytest

import zlink
from zlink._runtime.core import context as context_runtime
from zlink._runtime.eventing import poller as poller_runtime


def test_context_close_shuts_down_before_termination():
    context = zlink.create_context()
    calls = []
    real = context_runtime.lib()

    class Native:
        def __getattr__(self, name):
            target = getattr(real, name)
            if name not in {"zlink_ctx_shutdown", "zlink_ctx_term"}:
                return target

            def recorded(*args):
                calls.append(name)
                return target(*args)

            return recorded

    with patch.object(context_runtime, "lib", return_value=Native()):
        context.close()
    assert calls == ["zlink_ctx_shutdown", "zlink_ctx_term"]


def test_poller_busy_destroy_reports_typed_error_and_keeps_handle():
    poller = zlink.create_poller()
    real = poller_runtime.lib()
    attempts = 0

    class Native:
        def __getattr__(self, name):
            return getattr(real, name)

        def zlink_errno(self):
            return errno.EBUSY

        def zlink_poller_destroy(self, handle):
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                return int(zlink.CloseResult.BUSY)
            return real.zlink_poller_destroy(handle)

    handle = poller._handle
    with patch.object(poller_runtime, "lib", return_value=Native()):
        with pytest.raises(zlink.CloseError) as raised:
            poller.close()
        assert raised.value.result == zlink.CloseResult.BUSY
        assert raised.value.native_errno == errno.EBUSY
        assert poller._handle == handle
        assert poller.size() == 0
        poller.close()
    assert attempts == 2
    assert poller._handle is None
