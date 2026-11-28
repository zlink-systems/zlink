# SPDX-License-Identifier: MPL-2.0

from typing import List, Optional, Protocol, runtime_checkable

from ..eventing.codes import MonitorEventMask
from .codes import ReceiveFlowState
from .connection_attempt import ConnectionAttempt


@runtime_checkable
class _SocketContract(Protocol):
    """Operations common to every socket type."""

    def bind(self, endpoint):
        """Bind to a local transport address, such as ``tcp://*:5555``.

        Raises:
            ValueError: The endpoint contains a NUL or exceeds 255 UTF-8 bytes.
            BindError: Core could not bind the endpoint.
        """
        ...

    def unbind(self, endpoint: str):
        """Stop accepting connections at the bound endpoint.

        Raises:
            ValueError: The endpoint contains a NUL or exceeds 255 UTF-8 bytes.
            ConnectError: Core could not unbind the endpoint.
        """
        ...

    def set_tls_server(
        self, cert: str, key: str, require_client_cert: bool = False
    ):
        """Configure the server certificate, key, and client certificate policy.

        Call before binding.

        Raises:
            ValueError: The certificate or key path contains a NUL.
            ConfigError: Core rejected the TLS configuration.
        """
        ...

    def set_tls_client(
        self,
        ca_cert: Optional[str],
        hostname: Optional[str],
        trust_system: bool = False,
    ):
        """Configure the trusted CA, server name, and system CA policy.

        Call before connecting.

        Raises:
            ValueError: The CA path or server name contains a NUL.
            ConfigError: Core rejected the TLS configuration.
        """
        ...

    def monitor_open(
        self, events: MonitorEventMask = MonitorEventMask.ALL, monitor_hwm_bytes: int = 0
    ): ...

    def set_receive_flow_state(self, state: ReceiveFlowState): ...

    def close(self):
        """Close the socket and release its native resources."""
        ...

    def __enter__(self): ...

    def __exit__(self, exc_type, exc, tb): ...

    @property
    def options(self):
        """The typed options facade for this socket."""
        ...


@runtime_checkable
class _DisconnectRidContract(_SocketContract, Protocol):
    """Sockets that can disconnect a peer by its routing id."""

    def disconnect_rid(self, peer_rid):
        """Disconnect the peer identified by ``peer_rid`` rather than by
        address."""
        ...


@runtime_checkable
class _ConnectSocketContract(_DisconnectRidContract, Protocol):
    """Sockets that connect outward and report their connection attempts."""

    def connection_attempts_snapshot(self) -> List[ConnectionAttempt]:
        """Return an atomic snapshot of outbound connection registrations.

        A successful snapshot clears ``PollEventFlag.POLLCONNECT`` readiness
        unless a later change occurred during the snapshot.

        Raises:
            ConfigError: Core could not read the connection attempts.
        """
        ...


@runtime_checkable
class _RoutingIdContract(_SocketContract, Protocol):
    """Sockets whose own routing id can be set and read."""

    def set_routing_id(self, routing_id): ...

    def get_routing_id(self): ...


def __getattr__(name):
    if name in {"PairSocket", "DealerSocket"}:
        from . import message_socket_contracts

        value = getattr(message_socket_contracts, name)
    elif name == "RouterSocket":
        from .routed_socket_contracts import RouterSocket as value
    elif name in {"PubSocket", "SubSocket", "XPubSocket", "XSubSocket"}:
        from . import pubsub_socket_contracts

        value = getattr(pubsub_socket_contracts, name)
    elif name == "StreamSocket":
        from .stream_socket import StreamSocket as value
    else:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    globals()[name] = value
    return value
