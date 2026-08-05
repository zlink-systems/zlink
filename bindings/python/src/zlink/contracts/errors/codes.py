# SPDX-License-Identifier: MPL-2.0

from enum import IntEnum, IntFlag

class CloseResult(IntEnum):
    """The outcome of closing a socket or resource."""
    OK = 0
    BUSY = 401
    SHUTDOWN = 402
    INVALID_HANDLE = 403
    INTERNAL_ERROR = 404

class BindResult(IntEnum):
    """The outcome of binding to an endpoint."""
    OK = 0
    INVALID_ARGUMENT = 501
    ADDR_IN_USE = 502
    NOT_SUPPORTED = 503
    INVALID_HANDLE = 504
    INTERNAL_ERROR = 505

class ConnectResult(IntEnum):
    """The outcome of connecting to an endpoint."""
    OK = 0
    INVALID_ARGUMENT = 601
    NOT_SUPPORTED = 602
    INVALID_HANDLE = 603
    INTERNAL_ERROR = 604
    NOT_FOUND = 605
    CONFLICT = 606
    BUSY = 607
    AUTH_FAILED = 608

class ConfigResult(IntEnum):
    """The outcome of reading or applying a configuration option."""
    OK = 0
    INVALID_HANDLE = 701
    INVALID_ARGUMENT = 702
    NOT_SUPPORTED = 703
    INTERNAL_ERROR = 704
    INVALID_STATE = 705
    NOT_FOUND = 706
    CONFLICT = 707
    BUFFER_TOO_SMALL = 708
    BUSY = 709

class ErrorCode(IntEnum):
    """Native zlink-internal error codes (FSM, protocol, termination)."""
    EFSM = 156384763
    ENOCOMPATPROTO = 156384764
    ETERM = 156384765
    EMTHREAD = 156384766

class ProtocolError(IntEnum):
    """Wire-protocol error codes reported during a handshake."""
    ZMP_MALFORMED_COMMAND_HELLO = 0x10000013

class DisconnectReason(IntEnum):
    """Why a connection was disconnected."""
    UNKNOWN = 0
    HANDSHAKE_FAILED = 3
    TRANSPORT_ERROR = 4
    CTX_TERM = 5

__all__ = [
    "CloseResult",
    "BindResult",
    "ConnectResult",
    "ConfigResult",
    "ErrorCode",
    "ProtocolError",
    "DisconnectReason",
]
