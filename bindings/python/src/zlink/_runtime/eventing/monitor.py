# SPDX-License-Identifier: MPL-2.0

import ctypes

from ...contracts.core.options import AutoHwmRecalcReason
from ...contracts.errors.codes import CloseResult, ConfigResult
from ...contracts.errors.errors import CloseError, ConfigError, RecvError
from ...contracts.eventing.codes import MonitorEventMask
from ...contracts.eventing.monitor import MonitorEvent, MonitorStatus
from ...contracts.sockets.codes import RecvResult
from ..._native.ffi import (
    ZlinkMonitorEvent,
    ZlinkMonitorStatus,
    ZlinkSocketMonitorOpenOptions,
    lib,
)
from ..buffers.payload_buffers import _validated_uint64
from ..handles.native_support import (
    _raise_last_error,
    _raise_result_error,
    _routing_id_bytes,
)


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
        snd_pending_bytes=int(snapshot.snd_pending_bytes),
        rcv_pending_bytes=int(snapshot.rcv_pending_bytes),
        auto_hwm_enabled=bool(snapshot.auto_hwm_enabled),
        auto_hwm_profile=int(snapshot.auto_hwm_profile),
        auto_hwm_role=int(snapshot.auto_hwm_role),
        auto_hwm_policy_class=int(snapshot.auto_hwm_policy_class),
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
        flow_paused_connections=int(snapshot.flow_paused_connections),
        flow_pause_applied_total=int(snapshot.flow_pause_applied_total),
        flow_resume_applied_total=int(snapshot.flow_resume_applied_total),
        flow_state_stale_total=int(snapshot.flow_state_stale_total),
        flow_pause_duration_ms=int(snapshot.flow_pause_duration_ms),
    )


class NativeMonitorSocket:
    def __init__(self, handle):
        self._handle = handle
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
            connection_id=int(native.connection_id),
            transport_lane=int(native.transport_lane),
            flags=int(native.flags),
        )

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

    def recv(self, *, flags=0):
        native = ZlinkMonitorEvent()
        rc = lib().zlink_socket_monitor_recv(self._handle, ctypes.byref(native), flags)
        if rc == RecvResult.NO_DATA:
            return None
        if rc != 0:
            _raise_result_error(RecvError, RecvResult, rc, lib().zlink_errno())
        return self._decode_event(native)

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


def open_socket_monitor(
    socket, events=MonitorEventMask.ALL, monitor_hwm_bytes=0
):
    options = ZlinkSocketMonitorOpenOptions()
    options.events = int(events)
    options.monitor_hwm_bytes = _validated_uint64(
        monitor_hwm_bytes, field="monitor_hwm_bytes"
    )
    handle = lib().zlink_socket_monitor_open(socket._handle, ctypes.byref(options))
    return NativeMonitorSocket(handle)
