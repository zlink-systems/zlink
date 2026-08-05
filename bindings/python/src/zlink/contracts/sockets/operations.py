# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable


@runtime_checkable
class _FluentMessageOp(Protocol):
    """Build a multipart operation before submitting it."""

    def message(self, payload):
        """Add one payload part; successful submission consumes it."""
        ...

    def messages(self, *payloads):
        """Add payload parts in order; successful submission consumes them."""
        ...

    def flags(self, flags):
        """Set the send flags used by submission."""
        ...


@runtime_checkable
class SendOp(_FluentMessageOp, Protocol):
    """Build and submit a multipart send."""

    def submit(self):
        """Submit the parts and return the operation result."""
        ...


@runtime_checkable
class RequestOp(_FluentMessageOp, Protocol):
    """Build a request and deliver its reply to a callback."""

    def timeout(self, timeout):
        """Set the reply timeout."""
        ...

    def submit(self, callback):
        """Submit the request and deliver the reply to ``callback``."""
        ...


@runtime_checkable
class RequestCallbackOp(_FluentMessageOp, Protocol):
    """Request builder stage used after send flags have been selected."""

    def timeout(self, timeout):
        """Set the reply timeout."""
        ...

    def submit(self, callback):
        """Submit the request and deliver the reply to ``callback``."""
        ...


@runtime_checkable
class ReplyOp(_FluentMessageOp, Protocol):
    """Build and submit a reply."""

    def submit(self):
        """Submit the reply parts."""
        ...


__all__ = ["ReplyOp", "RequestCallbackOp", "RequestOp", "SendOp"]
