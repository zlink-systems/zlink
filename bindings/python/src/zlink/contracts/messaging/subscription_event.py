# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable


@runtime_checkable
class SubscriptionEvent(Protocol):
    """A subscriber's subscribe or unsubscribe as observed by an XPUB socket:
    its routing id, topic, and whether it subscribed."""


__all__ = ["SubscriptionEvent"]
