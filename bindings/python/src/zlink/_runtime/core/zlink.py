# SPDX-License-Identifier: MPL-2.0

import ctypes
import sys
import types

from ...contracts.errors.codes import CloseResult, ConfigResult
from ...contracts.errors.errors import CloseError, ConfigError
from ..._native.ffi import lib
from ..handles.native_support import _raise_result_error, _validated_int32


_THREAD_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p)


def _report_unhandled_exception(handler):
    exc_type, exc_value, exc_traceback = sys.exc_info()
    if exc_type is None:
        return
    sys.unraisablehook(
        types.SimpleNamespace(
            exc_type=exc_type,
            exc_value=exc_value,
            exc_traceback=exc_traceback,
            err_msg="Unhandled zlink callback exception",
            object=handler,
        )
    )


def _as_handle(value):
    return getattr(value, "_handle", value)


def version():
    major = ctypes.c_int()
    minor = ctypes.c_int()
    patch = ctypes.c_int()
    lib().zlink_version(ctypes.byref(major), ctypes.byref(minor), ctypes.byref(patch))
    return major.value, minor.value, patch.value


def strerror(code):
    value = lib().zlink_strerror(int(code))
    if not value:
        return ""
    return value.decode("utf-8", errors="replace")


def has(capability):
    return bool(lib().zlink_has(str(capability).encode("utf-8")))


def proxy(frontend, backend, capture=None):
    rc = lib().zlink_proxy(
        _as_handle(frontend),
        _as_handle(backend),
        None if capture is None else _as_handle(capture),
    )
    if rc != 0:
        _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())


def proxy_steerable(frontend, backend, capture, control):
    rc = lib().zlink_proxy_steerable(
        _as_handle(frontend),
        _as_handle(backend),
        _as_handle(capture),
        _as_handle(control),
    )
    if rc != 0:
        _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())


def sleep(seconds):
    lib().zlink_sleep(_validated_int32(seconds, field="seconds"))


def multipart_close(parts):
    for part in parts:
        part.close()


class NativeStopwatch:
    def __init__(self) -> None:
        if hasattr(self, "_handle"):
            return
        self._handle = lib().zlink_stopwatch_start()
        if not self._handle:
            _raise_result_error(ConfigError, ConfigResult, 701, lib().zlink_errno())

    def intermediate(self) -> int:
        if not self._handle:
            raise ConfigError(ConfigResult.INVALID_HANDLE, lib().zlink_errno())
        return int(lib().zlink_stopwatch_intermediate(self._handle))

    def stop(self) -> int:
        if not self._handle:
            raise ConfigError(ConfigResult.INVALID_HANDLE, lib().zlink_errno())
        handle = self._handle
        self._handle = None
        return int(lib().zlink_stopwatch_stop(handle))

    def close(self) -> None:
        if self._handle:
            self.stop()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.close()


class NativeThread:
    def __init__(self, target) -> None:
        if hasattr(self, "_handle"):
            return
        if target is None:
            raise ValueError("target must not be None")
        self._target = target
        self._callback = None

        def _invoke(_userdata):
            try:
                target()
            except Exception:
                _report_unhandled_exception(target)

        callback = _THREAD_FN(_invoke)
        self._callback = callback
        self._handle = lib().zlink_thread_start(ctypes.cast(callback, ctypes.c_void_p), None)
        if not self._handle:
            _raise_result_error(ConfigError, ConfigResult, 701, lib().zlink_errno())

    def join(self) -> None:
        if not self._handle:
            raise CloseError(CloseResult.INVALID_HANDLE, lib().zlink_errno())
        handle = self._handle
        self._handle = None
        lib().zlink_thread_join(handle)
        self._callback = None


class NativeAtomicCounter:
    def __init__(self) -> None:
        if hasattr(self, "_handle"):
            return
        self._handle = lib().zlink_atomic_counter_new()
        if not self._handle:
            _raise_result_error(ConfigError, ConfigResult, 701, lib().zlink_errno())

    def set(self, value: int) -> None:
        if not self._handle:
            raise ConfigError(ConfigResult.INVALID_HANDLE, lib().zlink_errno())
        lib().zlink_atomic_counter_set(self._handle, _validated_int32(value, field="value"))

    def increment(self) -> int:
        if not self._handle:
            raise ConfigError(ConfigResult.INVALID_HANDLE, lib().zlink_errno())
        return int(lib().zlink_atomic_counter_inc(self._handle))

    def decrement(self) -> int:
        if not self._handle:
            raise ConfigError(ConfigResult.INVALID_HANDLE, lib().zlink_errno())
        return int(lib().zlink_atomic_counter_dec(self._handle))

    @property
    def value(self) -> int:
        if not self._handle:
            raise ConfigError(ConfigResult.INVALID_HANDLE, lib().zlink_errno())
        return int(lib().zlink_atomic_counter_value(self._handle))

    def close(self) -> None:
        if not self._handle:
            return
        handle = ctypes.c_void_p(self._handle)
        self._handle = None
        lib().zlink_atomic_counter_destroy(ctypes.byref(handle))

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.close()


NativeStopwatch.__module__ = "zlink.contracts.core.utilities"
NativeStopwatch.__name__ = "Stopwatch"
NativeStopwatch.__qualname__ = "Stopwatch"
NativeThread.__module__ = "zlink.contracts.core.utilities"
NativeThread.__name__ = "Thread"
NativeThread.__qualname__ = "Thread"
NativeAtomicCounter.__module__ = "zlink.contracts.core.utilities"
NativeAtomicCounter.__name__ = "AtomicCounter"
NativeAtomicCounter.__qualname__ = "AtomicCounter"


def create_stopwatch():
    return NativeStopwatch()


def create_thread(target):
    return NativeThread(target)


def create_atomic_counter():
    return NativeAtomicCounter()
