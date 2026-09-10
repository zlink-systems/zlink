# SPDX-License-Identifier: MPL-2.0

from typing import Awaitable, Protocol, final, runtime_checkable

from ..messaging.message import Message
from .codes import SubmitResult


@final
class SendSubmission:
    """Initial send result and its admission stage."""

    __slots__ = ("_result", "_admitted")

    def __init__(self, result: SubmitResult, admitted: Awaitable[None]):
        self._result = result
        self._admitted = admitted

    @property
    def result(self) -> SubmitResult:
        return self._result

    @property
    def admitted(self) -> Awaitable[None]:
        return self._admitted


@final
class RequestSubmission:
    """Initial request result, admission stage, and reply stage."""

    __slots__ = ("_result", "_admitted", "_reply")

    def __init__(
        self,
        result: SubmitResult,
        admitted: Awaitable[None],
        reply: Awaitable[list["Message"]],
    ):
        self._result = result
        self._admitted = admitted
        self._reply = reply

    @property
    def result(self) -> SubmitResult:
        return self._result

    @property
    def admitted(self) -> Awaitable[None]:
        return self._admitted

    @property
    def reply(self) -> Awaitable[list["Message"]]:
        return self._reply


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

    def submit(self) -> SendSubmission:
        """Submit and return the initial result plus admission stage.

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
    """Build a routed request with managed admission backpressure."""

    def message(self, payload) -> "RequestOp": ...

    def messages(self, *payloads) -> "RequestOp": ...

    def timeout(self, timeout) -> "RequestOp":
        """Set the reply timeout."""
        ...

    def submit(self) -> RequestSubmission:
        """Submit and return separate admission and reply stages."""
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


__all__ = [
    "PublishOp",
    "ReplyOp",
    "RequestOp",
    "RequestSubmission",
    "SendOp",
    "SendSubmission",
]
