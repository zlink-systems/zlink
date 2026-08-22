# SPDX-License-Identifier: MPL-2.0

from enum import IntEnum, IntFlag

class MonitorEventMask(IntFlag):
    """A bitmask selecting which socket monitor events to subscribe to."""
    CONNECTED = 0x0001
    CONNECT_DELAYED = 0x0002
    CONNECT_RETRIED = 0x0004
    LISTENING = 0x0008
    BIND_FAILED = 0x0010
    ACCEPTED = 0x0020
    ACCEPT_FAILED = 0x0040
    CLOSED = 0x0080
    CLOSE_FAILED = 0x0100
    DISCONNECTED = 0x0200
    MONITOR_STOPPED = 0x0400
    HANDSHAKE_FAILED_NO_DETAIL = 0x0800
    CONNECTION_READY = 0x1000
    HANDSHAKE_FAILED_PROTOCOL = 0x2000
    HANDSHAKE_FAILED_AUTH = 0x4000
    PEER_WEIGHT_CHANGED = 0x8000
    SEND_FLOW_PAUSED = 0x10000
    SEND_FLOW_RESUMED = 0x20000
    FLOW_STATE_STALE = 0x40000
    ALL = 0x7FFFF

class MonitorEventFlag(IntFlag):
    """Event-specific bits carried in a monitor event's ``flags`` field.
    Mirrors ``ZLINK_MONITOR_EVENT_FLAG_*`` in the C ABI."""
    CONNECTION_READY_EDGE = 1 << 0
    SEND_FLOW_WRITABLE = 1 << 1
    FLOW_STATE_STALE_GENERATION = 1 << 2
    FLOW_STATE_STALE_EPOCH = 1 << 3

class MonitorStatusDetail(IntFlag):
    """Detail bits describing which ``MonitorStatus`` fields are populated.
    Mirrors ``zlink_monitor_status_detail_flag_e`` in the C ABI. Only the
    flow-state bit is named here; the raw ``MonitorStatus.detail_flags``
    field carries the others."""
    FLOW_STATE = 1 << 5

class PollEventFlag(IntFlag):
    """Readiness conditions a poll source can be watched for or report (readable, writable, error)."""
    """Mirrors ``zlink_poller_event_flag_e`` in the C ABI. ``POLLCOMPLETION``
    is reserved for binding runtime workers that drive request completion;
    application code generally uses ``POLLIN`` / ``POLLOUT``."""

    POLLIN = 1
    POLLOUT = 2
    POLLERR = 4
    POLLPRI = 8
    POLLITEMS_DFLT = 16
    POLLCOMPLETION = 32

class PollSourceKind(IntEnum):
    """Whether a poll event came from a socket, a file descriptor, or a timer."""
    SOCKET = 1
    FD = 2
    TIMER = 3

__all__ = [
    "MonitorEventMask",
    "MonitorEventFlag",
    "MonitorStatusDetail",
    "PollEventFlag",
    "PollSourceKind",
]
