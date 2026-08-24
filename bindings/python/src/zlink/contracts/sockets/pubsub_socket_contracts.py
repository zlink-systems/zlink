# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable

from . import socket as _socket_contract


@runtime_checkable
class PubSocket(_socket_contract._SocketContract, Protocol):
    """PUB socket: topic-filtered publish that drops messages with no matching
    subscriber."""

    @property
    def pub_options(self):
        """The PUB-specific typed options facade."""
        ...

    def connect(self, endpoint):
        """Connect to a remote transport address (asynchronous)."""
        ...

    def disconnect(self, endpoint):
        """Disconnect the connection previously established to ``endpoint``."""
        ...

    def publish(self, topic):
        """Begin publishing under ``topic``; parts are consumed on a successful
        submit."""
        ...


@runtime_checkable
class SubSocket(_socket_contract._SocketContract, Protocol):
    """SUB socket: a receive-only subscriber filtered by its subscriptions."""

    @property
    def sub_options(self):
        """The SUB-specific typed options facade."""
        ...

    def connect(self, endpoint):
        """Connect to a remote transport address (asynchronous)."""
        ...

    def disconnect(self, endpoint):
        """Disconnect the connection previously established to ``endpoint``."""
        ...

    def set_subscription(self, topic):
        """Add a subscription for ``topic`` (an exact topic or pattern);
        subscriptions accumulate."""
        ...

    def unset_subscription(self, topic):
        """Remove a subscription previously added for ``topic``."""
        ...

    def subscription_at(self, index):
        """Return ``(filter, is_pattern)`` for a subscription index, or
        ``None`` when the index is absent."""
        ...

    def subscribe_into(self, topic_message, *, flags=0):
        """Receive the next matching topic message into ``topic_message``
        storage; ``False`` when ``DONT_WAIT`` is set and none is available."""
        ...


@runtime_checkable
class XPubSocket(_socket_contract._SocketContract, Protocol):
    """XPUB socket: like PUB, but also surfaces subscriber subscription and
    unsubscription events."""

    @property
    def pub_options(self):
        """The PUB/XPUB-specific typed options facade."""
        ...

    def connect(self, endpoint):
        """Connect to a remote transport address (asynchronous)."""
        ...

    def disconnect(self, endpoint):
        """Disconnect the connection previously established to ``endpoint``."""
        ...

    def publish(self, topic):
        """Begin publishing under ``topic``; parts are consumed on a successful
        submit."""
        ...

    def receive_subscription_event_into(self, event, *, flags=0):
        """Receive the next subscriber (un)subscription event into ``event``
        storage; ``False`` when ``DONT_WAIT`` is set and none is available."""
        ...


@runtime_checkable
class XSubSocket(_socket_contract._SocketContract, Protocol):
    """XSUB socket: a subscriber whose subscriptions are carried as messages."""

    @property
    def sub_options(self):
        """The SUB-specific typed options facade."""
        ...

    def connect(self, endpoint):
        """Connect to a remote transport address (asynchronous)."""
        ...

    def disconnect(self, endpoint):
        """Disconnect the connection previously established to ``endpoint``."""
        ...

    def set_subscription(self, topic):
        """Add a subscription for ``topic``; subscriptions accumulate."""
        ...

    def unset_subscription(self, topic):
        """Remove a subscription previously added for ``topic``."""
        ...

    def subscription_at(self, index):
        """Return ``(filter, is_pattern)`` for a subscription index, or
        ``None`` when the index is absent."""
        ...

    def subscribe_into(self, topic_message, *, flags=0):
        """Receive the next matching topic message into ``topic_message``
        storage; ``False`` when ``DONT_WAIT`` is set and none is available."""
        ...


__all__ = ["PubSocket", "SubSocket", "XPubSocket", "XSubSocket"]
