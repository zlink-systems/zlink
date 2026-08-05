# SPDX-License-Identifier: MPL-2.0

import ctypes

from ...contracts.core.options import AutoHwmProfile, ContextOption
from ...contracts.errors.codes import CloseResult, ConfigResult
from ...contracts.errors.errors import CloseError, ConfigError
from ..._native.ffi import lib
from ..handles.native_support import (
    _config_result_from_errno,
    _raise_result_error,
    _raise_zlink_error,
    _validated_int32,
)


class NativeContext:
    def __init__(self):
        if hasattr(self, "_handle"):
            return
        self._handle = lib().zlink_ctx_new()
        if not self._handle:
            _raise_result_error(ConfigError, ConfigResult, 701, lib().zlink_errno())
        self._options = NativeContextOptions(self)

    @property
    def options(self):
        return self._options

    def _set_option(self, option, value):
        rc = lib().zlink_ctx_set(self._handle, int(option), _validated_int32(value))
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def _set_uint64_option(self, option, value):
        raw_value = ctypes.c_uint64(value)
        rc = lib().zlink_ctx_set_data(
            self._handle,
            int(option),
            ctypes.byref(raw_value),
            ctypes.sizeof(raw_value),
        )
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def _get_option(self, option):
        error_out = ctypes.c_int()
        value = lib().zlink_ctx_get(
            self._handle,
            int(option),
            ctypes.byref(error_out),
        )
        if value < 0:
            _raise_result_error(
                ConfigError,
                ConfigResult,
                error_out.value,
                lib().zlink_errno(),
            )
        return value

    def _get_uint64_option(self, option):
        raw_value = ctypes.c_uint64()
        value_size = ctypes.c_size_t(ctypes.sizeof(raw_value))
        rc = lib().zlink_ctx_get_data(
            self._handle,
            int(option),
            ctypes.byref(raw_value),
            ctypes.byref(value_size),
        )
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        return raw_value.value

    def recalculate_auto_hwm(self):
        rc = lib().zlink_ctx_auto_hwm_recalculate(self._handle)
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())

    def shutdown(self):
        rc = lib().zlink_ctx_shutdown(self._handle)
        if rc != 0:
            _raise_result_error(CloseError, CloseResult, rc, lib().zlink_errno())

    def close(self):
        if self._handle:
            rc = lib().zlink_ctx_term(self._handle)
            if rc != 0:
                _raise_result_error(CloseError, CloseResult, rc, lib().zlink_errno())
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.close()


class NativeContextOptions:
    def __init__(self, context):
        self._context = context
        self._thread_name_prefix = ""

    @property
    def io_threads(self):
        return self._context._get_option(ContextOption.IO_THREADS)

    @io_threads.setter
    def io_threads(self, value):
        self._context._set_option(ContextOption.IO_THREADS, value)

    @property
    def max_sockets(self):
        return self._context._get_option(ContextOption.MAX_SOCKETS)

    @max_sockets.setter
    def max_sockets(self, value):
        self._context._set_option(ContextOption.MAX_SOCKETS, value)

    @property
    def max_message_size(self):
        return self._context._get_option(ContextOption.MAX_MSGSZ)

    @max_message_size.setter
    def max_message_size(self, value):
        self._context._set_option(ContextOption.MAX_MSGSZ, value)

    @property
    def thread_scheduling_policy(self):
        return self._context._get_option(ContextOption.THREAD_SCHED_POLICY)

    @thread_scheduling_policy.setter
    def thread_scheduling_policy(self, value):
        self._context._set_option(ContextOption.THREAD_SCHED_POLICY, value)

    @property
    def thread_name_prefix(self):
        return self._thread_name_prefix

    @thread_name_prefix.setter
    def thread_name_prefix(self, value):
        if isinstance(value, str):
            raw = value.encode("utf-8")
        else:
            raw = bytes(value)
        rc = lib().zlink_ctx_set_data(
            self._context._handle,
            int(ContextOption.THREAD_NAME_PREFIX),
            ctypes.c_char_p(raw),
            len(raw),
        )
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        self._thread_name_prefix = (
            value if isinstance(value, str) else raw.decode("utf-8", errors="replace")
        )

    @property
    def auto_hwm_enabled(self):
        return bool(self._context._get_option(ContextOption.AUTO_HWM_ENABLE))

    @auto_hwm_enabled.setter
    def auto_hwm_enabled(self, value):
        self._context._set_option(ContextOption.AUTO_HWM_ENABLE, int(bool(value)))

    @property
    def auto_hwm_recalc_debounce(self):
        return self._context._get_option(ContextOption.AUTO_HWM_RECALC_DEBOUNCE_MS)

    @auto_hwm_recalc_debounce.setter
    def auto_hwm_recalc_debounce(self, value):
        self._context._set_option(ContextOption.AUTO_HWM_RECALC_DEBOUNCE_MS, value)

    @property
    def blocky(self):
        return bool(self._context._get_option(ContextOption.CTX_OPT_BLOCKY))

    @blocky.setter
    def blocky(self, value):
        self._context._set_option(ContextOption.CTX_OPT_BLOCKY, int(bool(value)))

    @property
    def auto_hwm_profile(self):
        return AutoHwmProfile(self._context._get_option(ContextOption.AUTO_HWM_PROFILE))

    @auto_hwm_profile.setter
    def auto_hwm_profile(self, value):
        self._context._set_option(ContextOption.AUTO_HWM_PROFILE, int(value))

    @property
    def auto_hwm_msg_unit_bytes(self):
        return self._context._get_uint64_option(ContextOption.AUTO_HWM_MSG_UNIT_BYTES)

    @auto_hwm_msg_unit_bytes.setter
    def auto_hwm_msg_unit_bytes(self, value):
        if value < 0:
            raise ValueError("auto_hwm_msg_unit_bytes must be non-negative")
        self._context._set_uint64_option(ContextOption.AUTO_HWM_MSG_UNIT_BYTES, value)

    @property
    def socket_limit(self):
        return self._context._get_option(ContextOption.SOCKET_LIMIT)

    @property
    def msg_t_size(self):
        return self._context._get_option(ContextOption.MSG_T_SIZE)

    def add_thread_affinity(self, cpu):
        self._context._set_option(ContextOption.THREAD_AFFINITY_CPU_ADD, cpu)

    def remove_thread_affinity(self, cpu):
        self._context._set_option(ContextOption.THREAD_AFFINITY_CPU_REMOVE, cpu)


NativeContext.__module__ = "zlink.contracts.core.context"
NativeContext.__name__ = "Context"
NativeContext.__qualname__ = "Context"
NativeContextOptions.__module__ = "zlink.contracts.core.context"
NativeContextOptions.__name__ = "ContextOptions"
NativeContextOptions.__qualname__ = "ContextOptions"


def create_context():
    return NativeContext()
