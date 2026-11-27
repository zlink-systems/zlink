# SPDX-License-Identifier: MPL-2.0

from typing import Optional, Protocol, runtime_checkable


@runtime_checkable
class SubscriptionEvent(Protocol):
    """A subscriber's subscribe or unsubscribe as observed by an XPUB socket:
    its routing id, topic, and whether it subscribed."""

    routing_id: Optional[bytes]
    topic: str
    subscribed: bool


__all__ = ["SubscriptionEvent"]
