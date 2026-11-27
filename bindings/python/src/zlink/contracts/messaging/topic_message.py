# SPDX-License-Identifier: MPL-2.0

from typing import Optional, Protocol, runtime_checkable

from ..core.routing_id import RoutingId
from .received import _BaseReceived


@runtime_checkable
class TopicMessage(_BaseReceived, Protocol):
    """A received publish: its topic and message parts. Owns its parts until
    closed."""

    routing_id: Optional[RoutingId]
    """The publisher's routing id when the receiving socket reports one,
    otherwise ``None``."""

    @property
    def topic(self):
        """The topic the message was published under."""
        ...

    @topic.setter
    def topic(self, value): ...


__all__ = ["TopicMessage"]
