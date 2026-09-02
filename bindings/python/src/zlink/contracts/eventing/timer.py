# SPDX-License-Identifier: MPL-2.0

from typing import Optional, Protocol, runtime_checkable


@runtime_checkable
class Timer(Protocol):
    """A timer that fires on an interval and can be polled or awaited."""

    def start(self, interval_ns: int, repeat_count: int) -> None:
        """Start the timer firing once per ``interval_ns`` nanoseconds;
        ``repeat_count`` sets how many times it fires."""
        ...

    def stop(self) -> None:
        """Stop the timer; it can be restarted with :meth:`start`."""
        ...

    def recv(self) -> Optional[int]:
        """Receive the next expiration as the cumulative fire count, or ``None``
        when none is pending."""
        ...

    def close(self) -> None:
        """Close the timer and release its resources."""
        ...

    def __enter__(self): ...

    def __exit__(self, exc_type, exc, tb): ...

    async def __aenter__(self): ...

    async def __aexit__(self, exc_type, exc, tb): ...
