# SPDX-License-Identifier: MPL-2.0

from dataclasses import dataclass
from typing import Protocol, Tuple, runtime_checkable


@dataclass(frozen=True)
class CoreHwmBudgetSnapshot:
    """Immutable Core Auto HWM ABI-v1 context budget snapshot."""

    abi_version: int
    struct_size: int
    budget_generation: int
    measurement_epoch: int
    configured_memory_limit_bytes: int
    runtime_memory_limit_bytes: int
    resolved_memory_limit_bytes: int
    configured_core_budget_bytes: int
    effective_core_budget_bytes: int
    total_planned_hwm_bytes: int
    total_applied_hwm_bytes: int
    manual_reserved_hwm_bytes: int
    core_queue_accounted_bytes: int
    application_accounted_bytes: int
    current_accounted_bytes: int
    provisional_accounted_bytes: int
    peak_accounted_bytes: int
    completion_current_accounted_bytes: int
    completion_peak_accounted_bytes: int
    completion_pending_message_count: int
    total_messaging_accounted_bytes: int
    monitor_queue_applied_hwm_bytes: int
    monitor_queue_accounted_bytes: int
    total_instance_applied_hwm_bytes: int
    total_instance_accounted_bytes: int
    oversize_admission_count: int
    largest_oversize_message_bytes: int
    active_directional_queue_count: int
    active_completion_directional_queue_count: int
    active_send_queue_count: int
    active_receive_queue_count: int
    outstanding_application_lease_count: int
    retired_queue_count: int
    deferred_origin_credit_bytes: int
    unlimited_manual_queue_count: int
    blocked_ratio_ppm: int
    flags: int
    reserved_u64: Tuple[int, ...]

    @property
    def budget_planning_active(self):
        return bool(self.flags & (1 << 0))

    @property
    def budget_insufficient(self):
        return bool(self.flags & (1 << 1))

    @property
    def aggregate_hwm_valid(self):
        return bool(self.flags & (1 << 2))

    @property
    def aggregate_overflow(self):
        return bool(self.flags & (1 << 3))


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

    def core_hwm_budget_snapshot(self):
        """Return Core's immutable context-wide HWM budget snapshot."""
        ...

    def reset_core_hwm_budget_metrics(self):
        """Reset epoch metrics while preserving current budget gauges."""
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
    def core_hwm_profile(self):
        """The profile used to divide the Core memory budget."""
        ...
    @core_hwm_profile.setter
    def core_hwm_profile(self, value): ...

    @property
    def core_hwm_memory_limit_bytes(self):
        """The explicit context memory limit in bytes."""
        ...
    @core_hwm_memory_limit_bytes.setter
    def core_hwm_memory_limit_bytes(self, value): ...

    @property
    def core_hwm_budget_bytes(self):
        """The explicit context-wide Core budget in bytes."""
        ...
    @core_hwm_budget_bytes.setter
    def core_hwm_budget_bytes(self, value): ...

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
