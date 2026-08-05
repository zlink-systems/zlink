# SPDX-License-Identifier: MPL-2.0

from typing import Protocol, runtime_checkable


@runtime_checkable
class Context(Protocol):
    """A messaging context: the factory and owner of sockets.

    Supports the context-manager protocol; closing it terminates anything still
    open under it.
    """

    @property
    def options(self):
        """The context-wide options facade."""
        ...

    def recalculate_auto_hwm(self):
        """Recompute automatic high-water marks for the context's sockets
        immediately."""
        ...

    def shutdown(self):
        """Terminate the context, interrupting blocking operations on its
        sockets without closing them."""
        ...

    def close(self):
        """Close the context and release its resources."""
        ...

    def __enter__(self): ...

    def __exit__(self, exc_type, exc, tb): ...

    async def __aenter__(self): ...

    async def __aexit__(self, exc_type, exc, tb): ...


@runtime_checkable
class ContextOptions(Protocol):
    """Context-wide options governing the I/O threads and defaults shared by
    every socket created from the context."""

    @property
    def io_threads(self):
        """The number of background I/O threads serving the context."""
        ...
    @io_threads.setter
    def io_threads(self, value): ...

    @property
    def max_sockets(self):
        """The maximum number of sockets the context may create."""
        ...
    @max_sockets.setter
    def max_sockets(self, value): ...

    @property
    def max_message_size(self):
        """The default maximum inbound message size, in bytes, for new
        sockets."""
        ...
    @max_message_size.setter
    def max_message_size(self, value): ...

    @property
    def thread_scheduling_policy(self):
        """The OS scheduling policy of the context's I/O threads."""
        ...
    @thread_scheduling_policy.setter
    def thread_scheduling_policy(self, value): ...

    @property
    def thread_name_prefix(self):
        """The prefix applied to the names of threads the context creates."""
        ...
    @thread_name_prefix.setter
    def thread_name_prefix(self, value): ...

    @property
    def auto_hwm_enabled(self):
        """Whether high-water marks are sized automatically."""
        ...
    @auto_hwm_enabled.setter
    def auto_hwm_enabled(self, value): ...

    @property
    def auto_hwm_recalc_debounce(self):
        """The minimum delay between automatic high-water-mark
        recalculations."""
        ...
    @auto_hwm_recalc_debounce.setter
    def auto_hwm_recalc_debounce(self, value): ...

    @property
    def blocky(self):
        """Whether the context blocks on termination until queued messages have
        been sent."""
        ...
    @blocky.setter
    def blocky(self, value): ...

    @property
    def auto_hwm_profile(self):
        """The profile used to size high-water marks automatically."""
        ...
    @auto_hwm_profile.setter
    def auto_hwm_profile(self, value): ...

    @property
    def auto_hwm_msg_unit_bytes(self):
        """The assumed message size, in bytes, used when auto-sizing high-water
        marks."""
        ...
    @auto_hwm_msg_unit_bytes.setter
    def auto_hwm_msg_unit_bytes(self, value): ...

    @property
    def socket_limit(self):
        """The largest value :attr:`max_sockets` may take on this build."""
        ...

    @property
    def msg_t_size(self):
        """The size of the context's message worker thread pool."""
        ...

    def add_thread_affinity(self, cpu):
        """Pin the context's I/O threads to also run on CPU core ``cpu``."""
        ...

    def remove_thread_affinity(self, cpu):
        """Remove CPU core ``cpu`` from the context's I/O thread affinity."""
        ...
