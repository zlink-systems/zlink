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
        """Wait for send admission.

        Raises:
            SubmitError: The send failed before admission.
        """
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
        """Wait for request admission.

        Raises:
            SubmitError: The request failed before admission.
        """
        return self._admitted

    @property
    def reply(self) -> Awaitable[list["Message"]]:
        """Wait for the caller-owned reply parts.

        Raises:
            SubmitError: The request failed before admission.
            RequestError: The admitted request failed.
        """
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

        Raises:
            RuntimeError: No asyncio event loop is running.
            SubmitError: Submission failed, including when admission would
                wait and no completion poller is registered.
        """
        ...

    def submit_sync(self) -> None:
        """Submit synchronously through Core's blocking admission path.

        Raises:
            SubmitError: Submission failed.
        """
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
        """Submit and return separate admission and reply stages.

        Raises:
            ValueError: The configured reply timeout is outside the supported range.
            RuntimeError: No asyncio event loop is running.
            SubmitError: Submission failed, including when no completion poller
                is registered.
        """
        ...

    def submit_sync(self) -> list["Message"]:
        """Block until the request reaches a terminal result.

        Raises:
            ValueError: The configured reply timeout is outside the supported range.
            SubmitError: Submission failed.
            RequestError: The admitted request failed.
            RecvError: The blocking completion read failed.
        """
        ...


@runtime_checkable
class ReplyOp(_FluentMessageOp, Protocol):
    """Build and submit a reply."""

    def message(self, payload) -> "ReplyOp": ...

    def messages(self, *payloads) -> "ReplyOp": ...

    def submit(self) -> None:
        """Submit the reply parts.

        Raises:
            SubmitError: Submission failed.
        """
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
        """Submit the publication synchronously.

        Raises:
            SubmitError: Submission failed.
        """
        ...


__all__ = [
    "PublishOp",
    "ReplyOp",
    "RequestOp",
    "RequestSubmission",
    "SendOp",
    "SendSubmission",
]
