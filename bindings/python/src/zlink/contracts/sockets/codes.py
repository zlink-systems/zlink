# SPDX-License-Identifier: MPL-2.0

from enum import IntEnum, IntFlag

class SocketType(IntEnum):
    """A socket's messaging pattern (``PAIR``, ``PUB``, ``DEALER``, ``ROUTER``,
    ``STREAM``, and so on)."""

    ANY = 0
    PAIR = 0x1001
    PUB = 0x1002
    SUB = 0x1003
    DEALER = 0x1004
    ROUTER = 0x1005
    XPUB = 0x1006
    XSUB = 0x1007
    STREAM = 0x1008

class SendFlags(IntEnum):
    """Flags that modify send behavior; ``DONT_WAIT`` reports back-pressure instead of blocking."""
    NONE = 0
    DONT_WAIT = 1

class RecvFlags(IntEnum):
    """Flags that modify receive behavior; ``DONT_WAIT`` returns instead of blocking when no message is available."""
    NONE = 0
    DONT_WAIT = 1

class SubmitResult(IntEnum):
    """The outcome of submitting a send or publish."""
    OK = 0
    BACKPRESSURED = 1
    NOT_CONNECTED = 2
    NOT_FOUND = 3
    TERMINATED = 4
    INVALID_HANDLE = 5
    INVALID_ARGUMENT = 6
    NOT_SUPPORTED = 7
    INVALID_STATE = 8
    THREAD_VIOLATION = 9
    OUT_OF_MEMORY = 10
    SEQ_EXHAUSTED = 11
    INTERNAL_ERROR = 12
    NOT_ADMITTED = 13

class RequestResult(IntEnum):
    """The outcome of a request, as delivered to a request callback."""
    OK = 0
    TIMED_OUT = 101
    NOT_FOUND = 102
    TERMINATED = 103
    PROTOCOL_ERROR = 104
    INTERNAL_ERROR = 105
    REJECTED = 106
    CONFLICT = 107
    BUSY = 108
    NOT_CONNECTED = 109
    INVALID_ARGUMENT = 110
    INVALID_STATE = 111
    NOT_SUPPORTED = 112
    BACKPRESSURED = 113

class RecvResult(IntEnum):
    """The outcome of a receive."""
    OK = 0
    NO_DATA = 201
    BUSY = 202
    TERMINATED = 203
    INVALID_HANDLE = 204
    NOT_SUPPORTED = 205
    INTERNAL_ERROR = 206
    BUFFER_TOO_SMALL = 207
    INVALID_STATE = 208

class HandlerResult(IntEnum):
    """The outcome of registering or running a callback handler."""
    OK = 0
    INVALID_ARGUMENT = 301
    BUSY = 302
    NOT_SUPPORTED = 303
    DEADLOCK = 304
    INVALID_HANDLE = 305
    INTERNAL_ERROR = 306

class RidDuplicatePolicy(IntEnum):
    """How a socket reacts to a peer that reuses an existing routing id."""
    REJECT = 0
    HANDOVER = 1

class SubmitRetryMode(IntEnum):
    """Whether a failed submit is retried (``OFF`` or ``LOCAL_FAILURE``)."""
    OFF = 0
    LOCAL_FAILURE = 1

__all__ = [
    "SocketType",
    "SendFlags",
    "RecvFlags",
    "SubmitResult",
    "RequestResult",
    "RecvResult",
    "HandlerResult",
    "RidDuplicatePolicy",
    "SubmitRetryMode",
]
