# SPDX-License-Identifier: MPL-2.0

from dataclasses import dataclass
from typing import Protocol, runtime_checkable

from .codes import PollEventFlag, PollSourceKind
from .monitor import MonitorSocket


@dataclass(frozen=True)
class PollEvent:
    """One ready source reported by :meth:`Poller.wait`.

    Attributes:
        source_kind: Whether the ready source is a socket, file descriptor, or timer.
        slot: The caller token supplied when the source was registered.
        revents: The events that fired, as a mask of poll flags.
        fd: The file descriptor, when the source is a raw fd.
    """

    source_kind: PollSourceKind
    slot: int
    revents: int
    fd: int = 0


@runtime_checkable
class PollEvents(Protocol):
    """A reusable buffer of poll results filled by :meth:`Poller.wait`."""

    @property
    def capacity(self):
        """The maximum number of results the buffer can hold."""
        ...

    @property
    def ready_count(self):
        """The number of ready sources written by the last wait."""
        ...

    def source_kind(self, index):
        """Return the source kind of the result at ``index``."""
        ...

    def slot(self, index):
        """Return the caller token of the result at ``index``."""
        ...

    def revents(self, index):
        """Return the fired-event mask of the result at ``index``."""
        ...

    def has_event(self, index, event):
        """Return whether the result at ``index`` includes ``event``."""
        ...

    def fd(self, index):
        """Return the file descriptor of the result at ``index``."""
        ...

    def event(self, index):
        """Return the result at ``index`` as a :class:`PollEvent`."""
        ...


@runtime_checkable
class Poller(Protocol):
    """Multiplexes sockets, socket monitors, file descriptors, and timers,
    reporting which become ready on a single wait."""

    def add_socket(self, socket, events, slot):
        """Register ``socket`` to be watched for ``events``; ``slot`` is echoed
        back in the matching result. Also accepts :class:`MonitorSocket` with
        ``POLLIN`` only; drain it with ``recv(flags=RecvFlags.DONT_WAIT)``."""
        ...

    def add_monitor(self, monitor: MonitorSocket, events: PollEventFlag, slot: int) -> None:
        """Register a monitor with ``POLLIN`` and a caller slot.

        Alias of ``add_socket``. Other readiness flags raise ``ConfigError``
        with ``ConfigResult.INVALID_ARGUMENT``. After readiness, drain
        ``monitor.recv(flags=RecvFlags.DONT_WAIT)`` until it returns ``None``.
        Remove the monitor before closing it.
        """
        ...

    def modify_monitor(self, monitor: MonitorSocket, events: PollEventFlag) -> None:
        """Alias of ``modify_socket``; monitors accept only ``POLLIN``."""
        ...

    def remove_monitor(self, monitor: MonitorSocket) -> None:
        """Alias of ``remove_socket``; unregister the borrowed monitor."""
        ...

    def add_fd(self, fd, events, slot):
        """Register raw file descriptor ``fd`` to be watched for ``events``;
        ``slot`` is echoed back in the matching result."""
        ...

    def add_timer(self, timer, slot):
        """Register ``timer``; its expirations surface as poll events tagged
        with ``slot``."""
        ...

    def modify_socket(self, socket, events):
        """Change watched events for a socket or monitor; monitors accept only ``POLLIN``."""
        ...

    def modify_fd(self, fd, events):
        """Change the watched events for an already-registered file
        descriptor."""
        ...

    def remove_socket(self, socket):
        """Unregister a socket or monitor."""
        ...

    def remove_fd(self, fd):
        """Unregister file descriptor ``fd``."""
        ...

    def remove_timer(self, timer):
        """Unregister ``timer``."""
        ...

    def size(self):
        """Return the number of registered sources."""
        ...

    def wait(self, events, timeout_ms):
        """Wait up to ``timeout_ms`` milliseconds for sources to become ready,
        filling ``events``; a negative timeout blocks indefinitely. Return the
        number of ready sources."""
        ...

    def close(self):
        """Close the poller and release its resources."""
        ...

    def __enter__(self): ...

    def __exit__(self, exc_type, exc, tb): ...

    async def __aenter__(self): ...

    async def __aexit__(self, exc_type, exc, tb): ...
