# SPDX-License-Identifier: MPL-2.0

from enum import IntEnum
from typing import NamedTuple


class ConnectionAttemptState(IntEnum):
    """Whether an outbound connection registration has a ready Core connection."""

    CONNECTING = 0
    CONNECTED = 1
    ATTEMPT_FAILED = 2


class ConnectionFailureKind(IntEnum):
    """Core's normalized failure cause."""

    NONE = 0
    REFUSED = 1
    ENDPOINT_MISSING = 2
    TIMED_OUT = 3
    UNREACHABLE = 4
    NAME_RESOLUTION = 5
    AUTH_FAILED = 6
    PROTOCOL_FAILED = 7
    CONNECTION_LOST = 8
    OTHER = 9


class ConnectionFailureStage(IntEnum):
    """The connection stage at which Core observed the failure."""

    NONE = 0
    RESOLVE = 1
    TRANSPORT = 2
    TLS = 3
    WEBSOCKET = 4
    CORE_HANDSHAKE = 5
    ESTABLISHED = 6


class ConnectionFailure(NamedTuple):
    """Core's typed failure. ``normalized_errno`` and ``disconnect_reason`` are
    diagnostic values; callers do not reclassify the cause."""

    kind: ConnectionFailureKind
    stage: ConnectionFailureStage
    normalized_errno: int
    disconnect_reason: int


class ConnectionAttempt(NamedTuple):
    """One current outbound connection registration. ``intent_id``,
    ``revision`` and ``last_failure_id`` are opaque tokens compared only for
    equality; ``last_failure_id`` is 0 while no failure was recorded."""

    endpoint: str
    intent_id: int
    revision: int
    last_failure_id: int
    state: ConnectionAttemptState
    last_failure: ConnectionFailure


__all__ = [
    "ConnectionAttempt",
    "ConnectionAttemptState",
    "ConnectionFailure",
    "ConnectionFailureKind",
    "ConnectionFailureStage",
]
