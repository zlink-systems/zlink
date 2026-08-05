# SPDX-License-Identifier: MPL-2.0

import ctypes
from typing import Optional

from ...contracts.errors.codes import CloseResult, ConfigResult
from ...contracts.errors.errors import CloseError, ConfigError, HandlerError, RecvError
from ...contracts.sockets.codes import HandlerResult, RecvResult
from ..._native.ffi import lib
from ..core.zlink import _report_unhandled_exception
from ..handles.native_support import _raise_result_error


_TIMER_HANDLER = ctypes.CFUNCTYPE(
    None,
    ctypes.c_void_p,
    ctypes.c_uint64,
    ctypes.c_void_p,
)


class NativeTimer:
    def __init__(self) -> None:
        if hasattr(self, "_handle"):
            return
        self._handle = lib().zlink_timer_new()
        if not self._handle:
            _raise_result_error(ConfigError, ConfigResult, 701, lib().zlink_errno())
        self._handler = None
        self._handler_cb = None

    def start(self, interval_ns: int, repeat_count: int) -> None:
        if not self._handle:
            raise ConfigError(ConfigResult.INVALID_HANDLE, lib().zlink_errno())
        rc = lib().zlink_timer_start(self._handle, int(interval_ns), int(repeat_count))
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def stop(self) -> None:
        if not self._handle:
            raise ConfigError(ConfigResult.INVALID_HANDLE, lib().zlink_errno())
        rc = lib().zlink_timer_stop(self._handle)
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def recv(self) -> Optional[int]:
        if not self._handle:
            raise RecvError(RecvResult.INVALID_HANDLE, lib().zlink_errno())
        fire_count = ctypes.c_uint64()
        rc = lib().zlink_timer_recv(self._handle, ctypes.byref(fire_count))
        if rc == RecvResult.NO_DATA:
            return None
        if rc != 0:
            _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
        return int(fire_count.value)

    def on_fire(self, handler) -> None:
        if handler is None:
            raise ValueError("handler must not be None")
        if self._handler_cb is not None:
            raise RuntimeError("handler is already attached")

        def _callback(_timer, fire_count, _userdata):
            try:
                handler(self, int(fire_count))
            except Exception:
                _report_unhandled_exception(handler)

        callback = _TIMER_HANDLER(_callback)
        rc = lib().zlink_timer_handler(self._handle, callback, None)
        if rc != 0:
            _raise_result_error(HandlerError, HandlerResult, rc, lib().zlink_errno())
        self._handler = handler
        self._handler_cb = callback

    def close(self) -> None:
        if not self._handle:
            return
        handle = ctypes.c_void_p(self._handle)
        rc = lib().zlink_timer_destroy(ctypes.byref(handle))
        if rc != 0:
            _raise_result_error(CloseError, CloseResult, rc, lib().zlink_errno())
        self._handle = None
        self._handler = None
        self._handler_cb = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.close()


NativeTimer.__module__ = "zlink.contracts.eventing.timer"
NativeTimer.__name__ = "Timer"
NativeTimer.__qualname__ = "Timer"


def create_timer():
    return NativeTimer()
