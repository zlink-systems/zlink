# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable


@runtime_checkable
class _SocketContract(Protocol):
    """Operations common to every socket type."""

    def bind(self, endpoint):
        """Bind to a local transport address (for example ``tcp://*:5555``) to
        accept connections there."""
        ...

    def close(self):
        """Close the socket and release its native resources."""
        ...

    def __enter__(self): ...

    def __exit__(self, exc_type, exc, tb): ...

    @property
    def options(self):
        """The typed options facade for this socket."""
        ...


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
