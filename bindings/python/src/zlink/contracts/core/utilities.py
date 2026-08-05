# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable


@runtime_checkable
class Stopwatch(Protocol):
    """A high-resolution elapsed-time stopwatch."""

    def intermediate(self) -> int:
        """Return the elapsed time so far, in microseconds, without stopping."""
        ...

    def stop(self) -> int:
        """Stop the stopwatch and return the total elapsed time in
        microseconds."""
        ...

    def close(self) -> None:
        """Release the stopwatch."""
        ...

    def __enter__(self): ...

    def __exit__(self, exc_type, exc, tb): ...

    async def __aenter__(self): ...

    async def __aexit__(self, exc_type, exc, tb): ...


@runtime_checkable
class Thread(Protocol):
    """A handle to a running background thread (see :func:`create_thread`)."""

    def join(self) -> None:
        """Block the caller until the thread finishes running its target."""
        ...


@runtime_checkable
class AtomicCounter(Protocol):
    """A thread-safe integer counter that can be shared across threads."""

    def set(self, value: int) -> None:
        """Atomically set the value."""
        ...

    def increment(self) -> int:
        """Atomically increment the counter and return the new value."""
        ...

    def decrement(self) -> int:
        """Atomically decrement the counter and return the new value."""
        ...

    @property
    def value(self) -> int:
        """The current value, read atomically."""
        ...

    def close(self) -> None:
        """Release the counter."""
        ...

    def __enter__(self): ...

    def __exit__(self, exc_type, exc, tb): ...

    async def __aenter__(self): ...

    async def __aexit__(self, exc_type, exc, tb): ...
