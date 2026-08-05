# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable

METADATA_KEY_USER_MIN = 0x0100
METADATA_VALUE_MAX = 65535


@runtime_checkable
class Message(Protocol):
    """A message payload.

    Supports the context-manager protocol; closing the message (or leaving its
    ``with`` block) releases the payload. Sending a message consumes it.
    """

    @classmethod
    def from_(cls, data):
        """Create a message holding an independent copy of ``data``."""
        raise NotImplementedError

    @classmethod
    def allocate(cls, size: int):
        """Allocate a message with writable payload storage of ``size`` bytes."""
        raise NotImplementedError

    def copy(self):
        """Return a new message holding an independent copy of this payload."""
        ...

    def size(self):
        """Return the payload size in bytes."""
        ...

    def is_empty(self):
        """Return ``True`` when the payload is empty."""
        ...

    @property
    def data(self):
        """A zero-copy ``memoryview`` of the payload, valid only while the
        message is open."""
        ...

    def to_bytes(self):
        """Return a new ``bytes`` object copying the payload."""
        ...

    def copy_to(self, destination, source_offset=0, destination_offset=0, length=None):
        """Copy the payload (or the ``length`` bytes from ``source_offset``)
        into ``destination`` at ``destination_offset``; return the number of
        bytes written."""
        ...

    def try_copy_to(self, destination):
        """Copy the payload into ``destination`` when it fits; return the number
        of bytes written, or ``None`` when ``destination`` is too small."""
        ...

    def to_string(self, encoding="utf-8"):
        """Decode the payload as text using ``encoding``."""
        ...

    def ref_count(self):
        """Return the native payload reference count (a diagnostic only; it does
        not affect ownership)."""
        ...

    def close(self):
        """Release the payload storage owned by this message."""
        ...

    def __enter__(self):
        """Enter the context manager, returning this message."""
        ...

    def __exit__(self, exc_type, exc, tb):
        """Close the message on leaving the ``with`` block."""
        ...

    async def __aenter__(self):
        """Enter the async context manager, returning this message."""
        ...

    async def __aexit__(self, exc_type, exc, tb):
        """Close the message on leaving the ``async with`` block."""
        ...


def __getattr__(name):
    if name in {"Received", "ReceivedMessage", "ReceivedMultipart"}:
        from . import received

        value = getattr(received, name)
    elif name == "TopicMessage":
        from .topic_message import TopicMessage as value
    elif name == "SubscriptionEvent":
        from .subscription_event import SubscriptionEvent as value
    else:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    globals()[name] = value
    return value
