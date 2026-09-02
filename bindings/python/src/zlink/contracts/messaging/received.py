# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

from typing import NoReturn, Optional, Protocol, final, runtime_checkable

from ..core.routing_id import RoutingId


@final
class ReplyToken:
    """Opaque capability for replying to one ROUTER request."""

    __slots__ = ("_owner", "_value")

    def __new__(cls) -> NoReturn:
        raise TypeError("ReplyToken is created by ROUTER request receive")

    def __setattr__(self, name, value) -> NoReturn:
        raise AttributeError("ReplyToken is immutable")

    def __eq__(self, other: object) -> bool:
        return (
            isinstance(other, ReplyToken)
            and self._owner is other._owner
            and self._value == other._value
        )

    def __hash__(self) -> int:
        return hash((id(self._owner), self._value))

    def __repr__(self) -> str:
        return "ReplyToken()"

    def __copy__(self):
        return self

    def __deepcopy__(self, memo):
        return self

    def __reduce_ex__(self, protocol):
        raise TypeError("ReplyToken cannot be serialized")


def _reply_token_from_native(owner, value):
    native_value = int(value)
    if owner is None or native_value == 0:
        raise ValueError("native reply token must have an owner and non-zero value")
    token = object.__new__(ReplyToken)
    object.__setattr__(token, "_owner", owner)
    object.__setattr__(token, "_value", native_value)
    return token


def _reply_token_owner_matches(token, owner):
    return isinstance(token, ReplyToken) and token._owner is owner


def _reply_token_value(token):
    return token._value


@runtime_checkable
class ReceivedMessage(Protocol):
    """A single received message part.

    Supports the context-manager protocol; closing it (or leaving its ``with``
    block) releases the part.
    """

    def __len__(self):
        """Return the part size in bytes."""
        ...

    @property
    def data(self):
        """A ``memoryview`` snapshot of the part payload."""
        ...

    def to_bytes(self):
        """Return a new ``bytes`` object copying the part."""
        ...

    def close(self):
        """Release the storage owned by this part."""
        ...

    def __enter__(self): ...

    def __exit__(self, exc_type, exc, tb): ...

    async def __aenter__(self): ...

    async def __aexit__(self, exc_type, exc, tb): ...


class _BaseReceived(Protocol):
    """Shared behavior for received multipart envelopes; owns its parts until
    closed."""

    def __iter__(self):
        ...

    def __len__(self):
        """Return the number of parts."""
        ...

    def to_bytes_list(self):
        """Return a list of ``bytes`` copying each part."""
        ...

    def is_single_part(self):
        """Return ``True`` when the envelope holds exactly one part."""
        ...

    def first_part(self):
        """Return the first part without transferring ownership; raise
        :class:`RecvError` when the envelope has no parts."""
        ...

    def single_part_or_throw(self):
        """Return the only part; raise :class:`RecvError` unless the envelope
        holds exactly one part."""
        ...

    def close(self):
        """Close every part, releasing their storage."""
        ...

    def __enter__(self):
        ...

    def __exit__(self, exc_type, exc, tb):
        ...

    async def __aenter__(self):
        ...

    async def __aexit__(self, exc_type, exc, tb):
        ...


@runtime_checkable
class ReceivedMultipart(_BaseReceived, Protocol):
    """A received multipart message: iterate or index its parts."""


@runtime_checkable
class Received(ReceivedMultipart, Protocol):
    """A received message envelope with routing metadata and an optional
    reply/send context."""

    routing_id: Optional[RoutingId]

    @property
    def reply_token(self) -> Optional[ReplyToken]:
        """The opaque reply capability for a ROUTER request, otherwise None."""
        ...

    def send(self) -> "SendOp":
        """Begin a send addressed to this envelope's source route; raise
        :class:`SubmitError` when the envelope carries no send context."""
        ...

    def reply(self) -> "ReplyOp":
        """Begin a reply to this request; raise :class:`SubmitError` unless the
        envelope is replyable (has a reply token)."""
        ...


__all__ = ["Received", "ReceivedMessage", "ReceivedMultipart", "ReplyToken"]
