# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable

from . import socket as _socket_contract


@runtime_checkable
class StreamSocket(_socket_contract._SocketContract, Protocol):
    """STREAM socket: exchanges framed packets with raw TCP peers."""

    @property
    def stream_options(self):
        """The STREAM-specific typed options facade."""
        ...

    def send(self, routing_id):
        """Begin a synchronous send addressed to ``routing_id``; ``submit()``
        preserves the existing bool result contract."""
        ...

    def send_async(self, routing_id):
        """Begin an HWM-managed send addressed to ``routing_id``. The returned
        routed operation exposes awaitable ``submit()`` and blocking
        ``submit_sync()`` terminals."""
        ...

    def recv_into(self, received, *, flags=0):
        """Receive a message into ``received`` storage; ``False`` when
        ``DONT_WAIT`` is set and none is available."""
        ...

    def on_packet(self, handler):
        """Register ``handler``, invoked for each inbound framed packet with the
        sender routing id, header, and body; it owns the two messages and runs
        on a background dispatch thread."""
        ...

    def disconnect_rid(self, peer_rid):
        """Disconnect the peer identified by ``peer_rid``."""
        ...


__all__ = ["StreamSocket"]
