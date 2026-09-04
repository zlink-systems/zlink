# SPDX-License-Identifier: MPL-2.0

from typing import Awaitable, Protocol, runtime_checkable

from ..messaging.message import Message


@runtime_checkable
class _FluentMessageOp(Protocol):
    """Build a multipart operation before submitting it."""

    def message(self, payload):
        """Add one payload part; successful submission consumes it."""
        ...

    def messages(self, *payloads):
        """Add payload parts in order; successful submission consumes them."""
        ...


@runtime_checkable
class SendOp(_FluentMessageOp, Protocol):
    """Build and submit a multipart send."""

    def message(self, payload) -> "SendOp": ...

    def messages(self, *payloads) -> "SendOp": ...

    def submit(self) -> Awaitable[None]:
        """Submit with event-loop-managed DONTWAIT backpressure retry.

        Each attempt is nonblocking. Immediate admission completes without a
        SEND completion; ``BACKPRESSURED``/``EAGAIN`` waits for the matching
        WRITABLE token before resubmitting the same packet.
        """
        ...

    def submit_sync(self) -> None:
        """Submit synchronously through Core's blocking admission path."""
        ...


@runtime_checkable
class RequestOp(_FluentMessageOp, Protocol):
    """Build an HWM-managed routed request."""

    def message(self, payload) -> "RequestOp": ...

    def messages(self, *payloads) -> "RequestOp": ...

    def timeout(self, timeout) -> "RequestOp":
        """Set the reply timeout."""
        ...

    def submit(self) -> Awaitable[list["Message"]]:
        """Return the coroutine that completes with the reply parts."""
        ...

    def submit_sync(self) -> list["Message"]:
        """Block until the request reaches a terminal result."""
        ...


@runtime_checkable
class ReplyOp(_FluentMessageOp, Protocol):
    """Build and submit a reply."""

    def message(self, payload) -> "ReplyOp": ...

    def messages(self, *payloads) -> "ReplyOp": ...

    def submit(self) -> None:
        """Submit the reply parts."""
        ...


@runtime_checkable
class PublishOp(_FluentMessageOp, Protocol):
    """Build and synchronously submit a topic publication."""

    def message(self, payload) -> "PublishOp": ...

    def messages(self, *payloads) -> "PublishOp": ...

    def flags(self, flags) -> "PublishOp":
        """Set the lossy/NODROP publication flags."""
        ...

    def submit(self) -> None:
        """Submit the publication synchronously."""
        ...


__all__ = ["PublishOp", "ReplyOp", "RequestOp", "SendOp"]
