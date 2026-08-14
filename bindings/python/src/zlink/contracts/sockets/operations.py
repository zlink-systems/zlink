# SPDX-License-Identifier: MPL-2.0

from typing import Awaitable, Protocol, runtime_checkable


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
class _FlaggedFluentMessageOp(_FluentMessageOp, Protocol):
    def flags(self, flags):
        """Set the send flags used by submission."""
        ...


@runtime_checkable
class SendOp(_FlaggedFluentMessageOp, Protocol):
    """Build and submit a multipart send."""

    def submit(self):
        """Submit the parts and return the operation result."""
        ...


@runtime_checkable
class RoutedSendOp(_FluentMessageOp, Protocol):
    """Build an HWM-managed DEALER or ROUTER send."""

    def submit(self) -> Awaitable[None]:
        """Return the coroutine that completes after exact-target admission."""
        ...


@runtime_checkable
class RequestOp(_FluentMessageOp, Protocol):
    """Build an HWM-managed routed request."""

    def timeout(self, timeout):
        """Set the reply timeout."""
        ...

    def submit(self) -> Awaitable[list]:
        """Return the coroutine that completes with the reply parts."""
        ...


@runtime_checkable
class ReplyOp(_FlaggedFluentMessageOp, Protocol):
    """Build and submit a reply."""

    def submit(self):
        """Submit the reply parts."""
        ...


__all__ = ["ReplyOp", "RequestOp", "RoutedSendOp", "SendOp"]
