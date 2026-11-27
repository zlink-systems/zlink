# SPDX-License-Identifier: MPL-2.0

from typing import Optional, Protocol, runtime_checkable

from ..core.routing_id import RoutingId


@runtime_checkable
class SubscriptionEvent(Protocol):
    """A subscribe or unsubscribe event received by an XPUB socket."""

    routing_id: Optional[RoutingId]
    """The subscriber's routing id when available, otherwise ``None``.

    Use ``bytes(routing_id)`` to obtain the raw bytes of a routing id.
    """
    topic: str
    subscribed: bool


__all__ = ["SubscriptionEvent"]
