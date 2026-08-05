# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable

from . import socket as _socket_contract


@runtime_checkable
class RouterSocket(_socket_contract._SocketContract, Protocol):
    """ROUTER socket: routes messages to peers addressed by routing id, the
    server side of asynchronous request/reply."""

    @property
    def router_options(self):
        """The ROUTER-specific typed options facade."""
        ...

    def connect(self, endpoint):
        """Connect to a remote transport address (asynchronous)."""
        ...

    def disconnect(self, endpoint):
        """Disconnect the connection previously established to ``endpoint``."""
        ...

    def send(self, routing_id):
        """Begin a send addressed to ``routing_id``; parts are consumed on a
        successful submit."""
        ...

    def request(self, routing_id):
        """Begin a request to peer ``routing_id``; parts are consumed on submit
        and a reply is awaited."""
        ...

    def reply(self, routing_id, request_seq):
        """Begin a reply to request ``request_seq`` from ``routing_id``; parts
        are consumed on a successful submit."""
        ...

    def recv_into(self, received, *, flags=0):
        """Receive a routed message into ``received`` storage; ``False`` when
        ``DONT_WAIT`` is set and none is available."""
        ...

__all__ = ["RouterSocket"]
