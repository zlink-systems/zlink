# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable

from . import socket as _socket_contract
from .operations import RequestOp, SendOp


@runtime_checkable
class PairSocket(_socket_contract._SocketContract, Protocol):
    """PAIR socket: an exclusive one-to-one peering with no routing."""

    def connect(self, endpoint):
        """Connect to a remote transport address. Connection is asynchronous;
        this returns before the peer is reachable."""
        ...

    def disconnect(self, endpoint):
        """Disconnect the connection previously established to ``endpoint``."""
        ...

    def send(self) -> SendOp:
        """Begin a multipart send: add parts on the returned builder, then
        submit. Parts are consumed on a successful submit."""
        ...

    def recv_into(self, received, *, flags=0):
        """Receive a message into ``received`` storage; return ``True`` on
        success and ``False`` when ``DONT_WAIT`` is set and none is
        available."""
        ...

    def disconnect_rid(self, peer_rid):
        """Disconnect the peer identified by ``peer_rid`` rather than by
        address."""
        ...


@runtime_checkable
class DealerSocket(_socket_contract._SocketContract, Protocol):
    """DEALER socket: load-balances sends across its connected peers and can
    issue routed requests."""

    @property
    def dealer_options(self):
        """The DEALER-specific typed options facade."""
        ...

    def connect(self, endpoint):
        """Connect to a remote transport address (asynchronous)."""
        ...

    def disconnect(self, endpoint):
        """Disconnect the connection previously established to ``endpoint``."""
        ...

    def send(self) -> SendOp:
        """Begin a multipart send; parts are consumed on a successful submit."""
        ...

    def request(self) -> RequestOp:
        """Begin a request: add parts, then submit and await a reply. Parts are
        consumed on a successful submit."""
        ...

    def recv_into(self, received, *, flags=0):
        """Receive a message into ``received`` storage; ``False`` when
        ``DONT_WAIT`` is set and none is available."""
        ...


__all__ = ["PairSocket", "DealerSocket"]
