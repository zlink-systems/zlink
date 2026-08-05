# SPDX-License-Identifier: MPL-2.0

import ctypes

from ...contracts.core.options import AutoHwmRecalcReason
from ...contracts.errors.codes import CloseResult, ConfigResult
from ...contracts.errors.errors import CloseError, ConfigError, HandlerError, RecvError
from ...contracts.eventing.codes import MonitorEventMask
from ...contracts.eventing.monitor import MonitorEvent, MonitorStatus
from ...contracts.sockets.codes import HandlerResult, RecvResult
from ..._native.ffi import (
    ZlinkMonitorEvent,
    ZlinkMonitorStatus,
    ZlinkSocketMonitorOpenOptions,
    lib,
)
from ..eventing.dispatcher import CallbackDispatcher
from ..handles.native_support import (
    _raise_last_error,
    _raise_result_error,
    _report_unhandled_callback_exception,
    _routing_id_bytes,
)
from ..sockets.socket_base import _enter_callback, _leave_callback


def _decode_fixed(buf):
    return bytes(buf).split(b"\0", 1)[0].decode("utf-8", errors="replace")


def _monitor_status_from_native(snapshot):
    return MonitorStatus(
        abi_version=int(snapshot.abi_version),
        struct_size=int(snapshot.struct_size),
        source_kind=int(snapshot.source_kind),
        state_flags=int(snapshot.state_flags),
        detail_flags=int(snapshot.detail_flags),
        snd_pending_msgs=int(snapshot.snd_pending_msgs),
        rcv_pending_msgs=int(snapshot.rcv_pending_msgs),
        auto_hwm_enabled=bool(snapshot.auto_hwm_enabled),
        auto_hwm_profile=int(snapshot.auto_hwm_profile),
        auto_hwm_role=int(snapshot.auto_hwm_role),
        auto_hwm_policy_class=int(snapshot.auto_hwm_policy_class),
        auto_hwm_unit_budget_bytes=int(snapshot.auto_hwm_unit_budget_bytes),
        auto_hwm_size_cap=int(snapshot.auto_hwm_size_cap),
        auto_hwm_socket_message_slots=int(snapshot.auto_hwm_socket_message_slots),
        auto_hwm_connection_bucket_enabled=bool(
            snapshot.auto_hwm_connection_bucket_enabled
        ),
        auto_hwm_connection_bucket_count=int(
            snapshot.auto_hwm_connection_bucket_count
        ),
        auto_hwm_connection_bucket_index=int(
            snapshot.auto_hwm_connection_bucket_index
        ),
        auto_hwm_connection_bucket_hwm_4k=int(
            snapshot.auto_hwm_connection_bucket_hwm_4k
        ),
        auto_hwm_connection_bucket_hysteresis_retained=bool(
            snapshot.auto_hwm_connection_bucket_hysteresis_retained
        ),
        auto_hwm_effective_message_bytes=int(snapshot.auto_hwm_effective_message_bytes),
        auto_hwm_planned_sndhwm_bytes=int(snapshot.auto_hwm_planned_sndhwm_bytes),
        auto_hwm_planned_rcvhwm_bytes=int(snapshot.auto_hwm_planned_rcvhwm_bytes),
        auto_hwm_applied_sndhwm_bytes=int(snapshot.auto_hwm_applied_sndhwm_bytes),
        auto_hwm_applied_rcvhwm_bytes=int(snapshot.auto_hwm_applied_rcvhwm_bytes),
        auto_hwm_effective_sndbuf=int(snapshot.auto_hwm_effective_sndbuf),
        auto_hwm_effective_rcvbuf=int(snapshot.auto_hwm_effective_rcvbuf),
        auto_hwm_last_recalc_ms=int(snapshot.auto_hwm_last_recalc_ms),
        auto_hwm_last_recalc_reason=AutoHwmRecalcReason(
            int(snapshot.auto_hwm_last_recalc_reason)
        ),
        auto_hwm_send_blocked_ratio_ppm=int(snapshot.auto_hwm_send_blocked_ratio_ppm),
        auto_hwm_deferred_sndhwm_bytes=int(snapshot.auto_hwm_deferred_sndhwm_bytes),
        auto_hwm_deferred_rcvhwm_bytes=int(snapshot.auto_hwm_deferred_rcvhwm_bytes),
        auto_hwm_deferred_sndhwm_valid=bool(snapshot.auto_hwm_deferred_sndhwm_valid),
        auto_hwm_deferred_rcvhwm_valid=bool(snapshot.auto_hwm_deferred_rcvhwm_valid),
        snd_bytes_in_flight=int(snapshot.snd_bytes_in_flight),
        rcv_bytes_in_flight=int(snapshot.rcv_bytes_in_flight),
        minimum_core_message_charge_bytes=int(snapshot.minimum_core_message_charge_bytes),
        oversize_message_admission_count=int(snapshot.oversize_message_admission_count),
        oversize_message_admission_max_bytes=int(
            snapshot.oversize_message_admission_max_bytes
        ),
    )


_SOCKET_MONITOR_HANDLER = ctypes.CFUNCTYPE(
    None, ctypes.POINTER(ZlinkMonitorEvent), ctypes.c_void_p
)


class NativeMonitorSocket:
    ignore_handler = staticmethod(lambda event: None)

    def __init__(self, handle):
        self._handle = handle
        self._handler = None
        self._handler_cb = None
        self._dispatcher = CallbackDispatcher(
            "zlink-monitor-dispatch", _enter_callback, _leave_callback
        )
        if not self._handle:
            _raise_last_error()

    @staticmethod
    def _decode_event(native):
        return MonitorEvent(
            event=int(native.event),
            value=int(native.value),
            routing_id=_routing_id_bytes(native.routing_id),
            local_addr=_decode_fixed(native.local_addr),
            remote_addr=_decode_fixed(native.remote_addr),
        )

    def _start_event_dispatch(self, handler):
        if handler is None:
            raise ValueError("handler must not be None")
        if self._handler is not None:
            raise RuntimeError("handler is already attached")

        self._handler = handler
        dispatcher = self._dispatcher
        decode = self._decode_event
        register = lib().zlink_socket_monitor_handler

        def _invoke(event):
            try:
                handler(event)
            except Exception:
                _report_unhandled_callback_exception(handler)

        def _callback(event_ptr, _):
            try:
                event = decode(event_ptr.contents)
            except Exception:
                _report_unhandled_callback_exception(handler)
                return
            dispatcher.submit(lambda event=event: _invoke(event))

        callback = _SOCKET_MONITOR_HANDLER(_callback)
        rc = register(self._handle, callback, None)
        if rc != 0:
            self._handler = None
            _raise_result_error(HandlerError, HandlerResult, rc, lib().zlink_errno())
        self._handler_cb = callback

    def status(self):
        snapshot = ZlinkMonitorStatus()
        rc = lib().zlink_monitor_status(self._handle, ctypes.byref(snapshot))
        if rc != 0:
            _raise_result_error(ConfigError, ConfigResult, rc, lib().zlink_errno())
        return _monitor_status_from_native(snapshot)

    def close(self):
        if not self._handle:
            return
        handle = ctypes.c_void_p(self._handle)
        rc = lib().zlink_monitor_close(ctypes.byref(handle))
        if rc != 0:
            _raise_result_error(CloseError, CloseResult, rc, lib().zlink_errno())
        self._handle = None
        self._handler = None
        self._handler_cb = None
        self._dispatcher.close()

    def recv(self, *, flags=0):
        native = ZlinkMonitorEvent()
        rc = lib().zlink_socket_monitor_recv(self._handle, ctypes.byref(native), flags)
        if rc == RecvResult.NO_DATA:
            return None
        if rc != 0:
            _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
        return self._decode_event(native)

    def on_event(self, handler):
        self._start_event_dispatch(handler)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.close()


NativeMonitorSocket.__module__ = "zlink.contracts.eventing.monitor"
NativeMonitorSocket.__name__ = "MonitorSocket"
NativeMonitorSocket.__qualname__ = "MonitorSocket"


def open_socket_monitor(socket, events=MonitorEventMask.ALL):
    options = ZlinkSocketMonitorOpenOptions()
    options.events = int(events)
    handle = lib().zlink_socket_monitor_open(socket._handle, ctypes.byref(options))
    return NativeMonitorSocket(handle)
