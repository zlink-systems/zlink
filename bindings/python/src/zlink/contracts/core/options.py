# SPDX-License-Identifier: MPL-2.0

from enum import IntEnum, IntFlag

class ContextOption(IntEnum):
    """Raw native context option codes used by the runtime."""
    IO_THREADS = 1
    MAX_SOCKETS = 2
    SOCKET_LIMIT = 3
    THREAD_PRIORITY = 3
    THREAD_SCHED_POLICY = 4
    MAX_MSGSZ = 5
    MSG_T_SIZE = 6
    THREAD_AFFINITY_CPU_ADD = 7
    THREAD_AFFINITY_CPU_REMOVE = 8
    THREAD_NAME_PREFIX = 9
    CTX_OPT_BLOCKY = 10
    AUTO_HWM_ENABLE = 12
    AUTO_HWM_RECALC_DEBOUNCE_MS = 14
    AUTO_HWM_PROFILE = 17
    AUTO_HWM_MSG_UNIT_BYTES = 18

class AutoHwmProfile(IntEnum):
    """An automatic high-water-mark sizing profile trading memory, latency, and throughput."""
    COMPACT = 0
    LOW_LATENCY = 1
    BALANCED = 2
    THROUGHPUT = 3

class AutoHwmRecalcReason(IntEnum):
    """What triggered the last automatic high-water-mark recalculation."""
    NONE = 0
    INITIAL = 1
    ROLE_CHANGE = 2
    POLICY_TOGGLE = 3
    REFRESH = 4
    DEFERRED_SHRINK = 5

__all__ = [
    "ContextOption",
    "AutoHwmProfile",
    "AutoHwmRecalcReason",
]
