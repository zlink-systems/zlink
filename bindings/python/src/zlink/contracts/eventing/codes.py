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
    Mirrors ``zlink_monitor_status_detail_flag_e`` in the C ABI."""
    SND_PENDING_MSGS = 1 << 1
    RCV_PENDING_MSGS = 1 << 2
    AUTO_HWM_BUDGET = 1 << 3
    AUTO_HWM_BUFFERS = 1 << 4
    FLOW_STATE = 1 << 5

class MonitorStateFlag(IntFlag):
    """State bits of ``MonitorStatus.state_flags``. Mirrors
    ``zlink_monitor_state_flag_e`` in the C ABI."""
    READY = 1 << 0
    BOUND_READY = 1 << 1
    CLOSED = 1 << 3

class MonitorSourceKind(IntEnum):
    """The kind of source a ``MonitorStatus`` describes."""
    SOCKET = 1

class TransportLane(IntEnum):
    """The physical connection lane a monitor event belongs to."""
    APPLICATION = 0
    COMPLETION = 1

class PollEventFlag(IntFlag):
    """Conditions a poll source can be watched for or report.

    Mirrors ``zlink_poller_event_flag_e`` in the C ABI. ``POLLCOMPLETION`` lets
    a public caller transfer completion dispatch ownership to the thread calling
    ``Poller.wait``; ordinary receive/send readiness uses ``POLLIN`` / ``POLLOUT``. ``POLLROUTE``
    reports a ROUTER selected-route change and ``POLLCONNECT`` a connection
    attempt state change (see ``connection_attempts_snapshot``).
    """

    POLLIN = 1
    POLLOUT = 2
    POLLERR = 4
    POLLPRI = 8
    POLLITEMS_DFLT = 16
    POLLCOMPLETION = 32
    POLLROUTE = 64
    POLLCONNECT = 128

class PollSourceKind(IntEnum):
    """Whether a poll event came from a socket, a file descriptor, or a timer."""
    SOCKET = 1
    FD = 2
    TIMER = 3

__all__ = [
    "MonitorEventMask",
    "MonitorEventFlag",
    "MonitorStatusDetail",
    "MonitorStateFlag",
    "MonitorSourceKind",
    "TransportLane",
    "PollEventFlag",
    "PollSourceKind",
]
