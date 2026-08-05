# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable


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

    def send(self):
        """Begin a send addressed to this envelope's source route; raise
        :class:`SubmitError` when the envelope carries no send context."""
        ...

    def reply(self):
        """Begin a reply to this request; raise :class:`SubmitError` unless the
        envelope is replyable (has a request sequence)."""
        ...


__all__ = ["Received", "ReceivedMessage", "ReceivedMultipart"]
