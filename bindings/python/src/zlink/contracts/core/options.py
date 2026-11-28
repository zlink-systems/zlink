# SPDX-License-Identifier: MPL-2.0

from enum import IntEnum

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
    "AutoHwmProfile",
    "AutoHwmRecalcReason",
]
