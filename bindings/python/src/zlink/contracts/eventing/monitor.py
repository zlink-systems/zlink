# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable

from .codes import MonitorEventMask


class MonitorStatus:
    """A point-in-time snapshot of a monitored socket's state and auto-high-
    water-mark telemetry.

    ``source_kind`` identifies the monitored source; ``state_flags`` and
    ``detail_flags`` are bit masks describing its current state;
    ``snd_pending_msgs``/``rcv_pending_msgs`` and their byte counterparts are
    the queued counts; and
    the ``auto_hwm_*`` fields report the automatic high-water-mark sizing
    decisions (applied marks, effective buffers, last recalculation, and
    deferred shrinks); and (since ABI 4) the ``flow_*`` fields report paired
    DEALER/ROUTER completion-lane receive-flow telemetry, populated only when
    ``detail_flags`` has the flow-state detail bit set.
    """

    def __init__(
        self,
        *,
        abi_version=None,
        struct_size=None,
        source_kind,
        state_flags,
        detail_flags,
        snd_pending_msgs,
        rcv_pending_msgs,
        snd_pending_bytes,
        rcv_pending_bytes,
        auto_hwm_enabled=None,
        auto_hwm_profile=None,
        auto_hwm_role=None,
        auto_hwm_policy_class=None,
        auto_hwm_planned_sndhwm_bytes=None,
        auto_hwm_planned_rcvhwm_bytes=None,
        auto_hwm_applied_sndhwm_bytes=None,
        auto_hwm_applied_rcvhwm_bytes=None,
        auto_hwm_effective_sndbuf=None,
        auto_hwm_effective_rcvbuf=None,
        auto_hwm_last_recalc_ms=None,
        auto_hwm_last_recalc_reason=None,
        auto_hwm_send_blocked_ratio_ppm=None,
        auto_hwm_deferred_sndhwm_bytes=None,
        auto_hwm_deferred_rcvhwm_bytes=None,
        auto_hwm_deferred_sndhwm_valid=None,
        auto_hwm_deferred_rcvhwm_valid=None,
        snd_bytes_in_flight=None,
        rcv_bytes_in_flight=None,
        minimum_core_message_charge_bytes=None,
        oversize_message_admission_count=None,
        oversize_message_admission_max_bytes=None,
        flow_paused_connections=None,
        flow_pause_applied_total=None,
        flow_resume_applied_total=None,
        flow_state_stale_total=None,
        flow_pause_duration_ms=None,
    ):
        self.abi_version = abi_version
        self.struct_size = struct_size
        self.source_kind = source_kind
        self.state_flags = state_flags
        self.detail_flags = detail_flags
        self.snd_pending_msgs = snd_pending_msgs
        self.rcv_pending_msgs = rcv_pending_msgs
        self.snd_pending_bytes = snd_pending_bytes
        self.rcv_pending_bytes = rcv_pending_bytes
        self.auto_hwm_enabled = auto_hwm_enabled
        self.auto_hwm_profile = auto_hwm_profile
        self.auto_hwm_role = auto_hwm_role
        self.auto_hwm_policy_class = auto_hwm_policy_class
        self.auto_hwm_planned_sndhwm_bytes = auto_hwm_planned_sndhwm_bytes
        self.auto_hwm_planned_rcvhwm_bytes = auto_hwm_planned_rcvhwm_bytes
        self.auto_hwm_applied_sndhwm_bytes = auto_hwm_applied_sndhwm_bytes
        self.auto_hwm_applied_rcvhwm_bytes = auto_hwm_applied_rcvhwm_bytes
        self.auto_hwm_effective_sndbuf = auto_hwm_effective_sndbuf
        self.auto_hwm_effective_rcvbuf = auto_hwm_effective_rcvbuf
        self.auto_hwm_last_recalc_ms = auto_hwm_last_recalc_ms
        self.auto_hwm_last_recalc_reason = auto_hwm_last_recalc_reason
        self.auto_hwm_send_blocked_ratio_ppm = auto_hwm_send_blocked_ratio_ppm
        self.auto_hwm_deferred_sndhwm_bytes = auto_hwm_deferred_sndhwm_bytes
        self.auto_hwm_deferred_rcvhwm_bytes = auto_hwm_deferred_rcvhwm_bytes
        self.auto_hwm_deferred_sndhwm_valid = auto_hwm_deferred_sndhwm_valid
        self.auto_hwm_deferred_rcvhwm_valid = auto_hwm_deferred_rcvhwm_valid
        self.snd_bytes_in_flight = snd_bytes_in_flight
        self.rcv_bytes_in_flight = rcv_bytes_in_flight
        self.minimum_core_message_charge_bytes = minimum_core_message_charge_bytes
        self.oversize_message_admission_count = oversize_message_admission_count
        self.oversize_message_admission_max_bytes = oversize_message_admission_max_bytes
        self.flow_paused_connections = flow_paused_connections
        self.flow_pause_applied_total = flow_pause_applied_total
        self.flow_resume_applied_total = flow_resume_applied_total
        self.flow_state_stale_total = flow_state_stale_total
        self.flow_pause_duration_ms = flow_pause_duration_ms

    def is_ready(self):
        """Return whether the monitored socket is in the ready state."""
        return bool(self.state_flags & (1 << 0))


class MonitorEvent:
    """A single socket connection-lifecycle event reported by a monitor.

    ``event`` is the event kind; ``value`` is an event-specific value such as an
    error code or reconnect interval; ``routing_id`` is the peer routing id when
    the event carries one; and ``local_addr``/``remote_addr`` are the endpoint
    addresses.
    """

    def __init__(self, *, event, value, routing_id, local_addr, remote_addr):
        self.event = event
        self.value = value
        self.routing_id = routing_id
        self.local_addr = local_addr
        self.remote_addr = remote_addr

@runtime_checkable
class MonitorSocket(Protocol):
    """Observes a socket's connection lifecycle events and current status."""

    ignore_handler = staticmethod(lambda event: None)

    def status(self):
        """Return a :class:`MonitorStatus` snapshot of the monitored socket."""
        ...

    def close(self):
        """Close the monitor and release its resources."""
        ...

    def recv(self, *, flags=0):
        """Receive the next monitor event, or ``None`` when ``DONT_WAIT`` is set
        and none is available."""
        ...

    def on_event(self, handler):
        """Register ``handler``, invoked for each monitor event on a background
        dispatch thread."""
        ...

    def __enter__(self): ...

    def __exit__(self, exc_type, exc, tb): ...

    async def __aenter__(self): ...

    async def __aexit__(self, exc_type, exc, tb): ...
