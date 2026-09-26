# SPDX-License-Identifier: MPL-2.0

from typing import List, NamedTuple, Protocol, runtime_checkable

from . import socket as _socket_contract
from .operations import ReplyOp, RequestOp, SendOp
from ..core.routing_id import RoutingId


class RouterRoute(NamedTuple):
    """One row of a ROUTER selected-route snapshot: the peer routing id and
    the generation of the route Core currently selects for it.

    ``route_generation`` is a nonzero opaque equality token; it changes
    whenever Core selects a different route for the same routing id. Compare
    it only for equality.
    """

    routing_id: RoutingId
    route_generation: int


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

    def send(self, routing_id) -> SendOp:
        """Begin a send addressed to ``routing_id``; parts are consumed on a
        successful submit."""
        ...

    def request(self, routing_id) -> RequestOp:
        """Begin a request to peer ``routing_id``; parts are consumed on submit
        and a reply is awaited."""
        ...

    def reply(self, routing_id, token) -> ReplyOp:
        """Begin a reply using ``token`` from ``routing_id``; parts
        are consumed on a successful submit."""
        ...

    def recv_into(self, received, *, flags=0):
        """Receive a routed message into ``received`` storage; ``False`` when
        ``DONT_WAIT`` is set and none is available."""
        ...

    def routes_snapshot(self) -> List[RouterRoute]:
        """Return the Core-selected route of every routing id as one atomic
        snapshot. A routing id without a row has no selected route. A
        successful snapshot clears ``PollEventFlag.POLLROUTE`` readiness
        unless a later change raced with it. Call it only from the socket's
        single route observer."""
        ...

__all__ = ["RouterRoute", "RouterSocket"]
