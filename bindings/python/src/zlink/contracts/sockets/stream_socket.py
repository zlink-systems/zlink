# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable

from ..core.routing_id import RoutingId
from ..messaging.received import Received
from ..messaging.stream_packet import StreamPacket
from . import socket as _socket_contract
from .codes import RecvFlags
from .operations import SendOp


@runtime_checkable
class StreamSocket(_socket_contract._SocketContract, Protocol):
    """STREAM socket: exchanges framed packets with raw TCP peers."""

    @property
    def stream_options(self):
        """The STREAM-specific typed options facade."""
        ...

    def send(self, routing_id: RoutingId) -> SendOp:
        """Begin a send addressed to ``routing_id``."""
        ...

    def recv_into(
        self, received: Received, *, flags: RecvFlags = RecvFlags.NONE
    ) -> bool:
        """Receive a message into ``received`` storage; ``False`` when
        ``DONT_WAIT`` is set and none is available."""
        ...

    def recv_packet_into(
        self, out: StreamPacket, *, flags: RecvFlags = RecvFlags.NONE
    ) -> bool:
        """Receive one framed packet into reusable caller-owned storage."""
        ...

    def disconnect_rid(self, peer_rid):
        """Disconnect the peer identified by ``peer_rid``."""
        ...


__all__ = ["StreamSocket"]
