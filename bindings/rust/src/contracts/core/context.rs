use std::time::Duration;

use crate::error::{CloseError, ConfigError};
use crate::internal::ContextStorage;
use crate::socket_contracts::{
    DealerSocket, PairSocket, PubSocket, RouterSocket, StreamSocket, SubSocket, XPubSocket,
    XSubSocket,
};

/// Selects an automatic high-water-mark sizing profile that trades memory,
/// latency, and throughput.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum AutoHwmProfile {
    /// Smallest queues, minimizing memory use.
    Compact,
    /// Small queues that drain quickly to favor latency.
    LowLatency,
    /// Balances latency against throughput.
    Balanced,
    /// Large queues that favor throughput.
    Throughput,
}

/// Recalculation trigger reported by the auto-HWM monitor snapshot.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum AutoHwmRecalcReason {
    /// No recalculation has occurred.
    None,
    /// The initial sizing at startup.
    Initial,
    /// The socket's role changed.
    RoleChange,
    /// The auto-HWM policy was toggled.
    PolicyToggle,
    /// A periodic refresh.
    Refresh,
    /// A shrink that was deferred to avoid disruption.
    DeferredShrink,
}

/// Context-wide Core HWM budget snapshot copied from ABI v1 without unit
/// conversion. Completion counters are diagnostic and are excluded from the
/// application queue budget and directional queue counts.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CoreHwmBudgetSnapshot {
    pub abi_version: u32,
    pub struct_size: u32,
    pub budget_generation: u64,
    pub measurement_epoch: u64,
    pub configured_memory_limit_bytes: u64,
    pub runtime_memory_limit_bytes: u64,
    pub resolved_memory_limit_bytes: u64,
    pub configured_core_budget_bytes: u64,
    pub effective_core_budget_bytes: u64,
    pub total_planned_hwm_bytes: u64,
    pub total_applied_hwm_bytes: u64,
    pub manual_reserved_hwm_bytes: u64,
    pub core_queue_accounted_bytes: u64,
    pub application_accounted_bytes: u64,
    pub current_accounted_bytes: u64,
    pub provisional_accounted_bytes: u64,
    pub peak_accounted_bytes: u64,
    pub completion_current_accounted_bytes: u64,
    pub completion_peak_accounted_bytes: u64,
    pub completion_pending_message_count: u64,
    pub total_messaging_accounted_bytes: u64,
    pub monitor_queue_applied_hwm_bytes: u64,
    pub monitor_queue_accounted_bytes: u64,
    pub total_instance_applied_hwm_bytes: u64,
    pub total_instance_accounted_bytes: u64,
    pub oversize_admission_count: u64,
    pub largest_oversize_message_bytes: u64,
    pub active_directional_queue_count: u64,
    pub active_completion_directional_queue_count: u64,
    pub active_send_queue_count: u64,
    pub active_receive_queue_count: u64,
    pub outstanding_application_lease_count: u64,
    pub retired_queue_count: u64,
    pub deferred_origin_credit_bytes: u64,
    pub unlimited_manual_queue_count: u64,
    pub blocked_ratio_ppm: u32,
    pub flags: u32,
    pub reserved_u64: [u64; 8],
}

impl CoreHwmBudgetSnapshot {
    pub fn budget_planning_active(&self) -> bool {
        self.flags & (1 << 0) != 0
    }

    pub fn budget_insufficient(&self) -> bool {
        self.flags & (1 << 1) != 0
    }

    pub fn aggregate_hwm_valid(&self) -> bool {
        self.flags & (1 << 2) != 0
    }

    pub fn aggregate_overflow(&self) -> bool {
        self.flags & (1 << 3) != 0
    }
}

/// The zlink context, the foundation for creating sockets and managing native
/// runtime resources.
///
/// The public type owns context lifetime. Native context handles and option
/// marshalling stay inside the private runtime implementation.
///
/// `Context` is `Send` and `Sync`. It may be shared across threads, for example
/// with [`std::sync::Arc`], and those threads may create sockets from the same
/// context. The context is terminated when the last owning `Context` value is
/// dropped, so callers must keep an owner alive while another thread is creating
/// or using sockets.
pub struct Context {
    pub(crate) inner: Box<ContextStorage>,
}

impl Context {
    /// Create a new context with default settings.
    pub fn new() -> Result<Self, ConfigError> {
        crate::ctx::context_new()
    }

    /// Create a PAIR socket.
    pub fn pair_socket(&self) -> Result<PairSocket, ConfigError> {
        crate::ctx::context_pair_socket(self)
    }

    /// Create a PUB socket.
    pub fn pub_socket(&self) -> Result<PubSocket, ConfigError> {
        crate::ctx::context_pub_socket(self)
    }

    /// Create a SUB socket.
    pub fn sub_socket(&self) -> Result<SubSocket, ConfigError> {
        crate::ctx::context_sub_socket(self)
    }

    /// Create a DEALER socket.
    pub fn dealer_socket(&self) -> Result<DealerSocket, ConfigError> {
        crate::ctx::context_dealer_socket(self)
    }

    /// Create a ROUTER socket.
    pub fn router_socket(&self) -> Result<RouterSocket, ConfigError> {
        crate::ctx::context_router_socket(self)
    }

    /// Create an XPUB socket.
    pub fn xpub_socket(&self) -> Result<XPubSocket, ConfigError> {
        crate::ctx::context_xpub_socket(self)
    }

    /// Create an XSUB socket.
    pub fn xsub_socket(&self) -> Result<XSubSocket, ConfigError> {
        crate::ctx::context_xsub_socket(self)
    }

    /// Create a STREAM socket.
    pub fn stream_socket(&self) -> Result<StreamSocket, ConfigError> {
        crate::ctx::context_stream_socket(self)
    }

    /// Interrupt all blocking calls on sockets of this context with ETERM.
    pub fn shutdown(&self) -> Result<(), CloseError> {
        self.inner.shutdown()
    }

    /// Recalculate and apply auto HWM for the entire context immediately.
    pub fn recalculate_auto_hwm(&self) -> Result<(), ConfigError> {
        self.inner.recalculate_auto_hwm()
    }

    /// Returns a value snapshot of Core's context-wide HWM budget state.
    pub fn core_hwm_budget_snapshot(&self) -> Result<CoreHwmBudgetSnapshot, ConfigError> {
        self.inner.core_hwm_budget_snapshot()
    }

    /// Resets epoch counters while preserving current HWM budget gauges.
    pub fn reset_core_hwm_budget_metrics(&self) -> Result<(), ConfigError> {
        self.inner.reset_core_hwm_budget_metrics()
    }

    /// Access typed context options.
    pub fn options(&self) -> ContextOptions<'_> {
        ContextOptions::new(self.inner.as_ref())
    }
}

/// Typed facade over context options.
pub struct ContextOptions<'a> {
    context: &'a ContextStorage,
}

impl<'a> ContextOptions<'a> {
    pub(crate) fn new(context: &'a ContextStorage) -> Self {
        Self { context }
    }

    /// Returns the number of background I/O threads serving the context.
    pub fn io_threads(&self) -> Result<i32, ConfigError> {
        self.context.io_threads()
    }
    /// Sets the number of background I/O threads serving the context.
    pub fn set_io_threads(&self, threads: i32) -> Result<(), ConfigError> {
        self.context.set_io_threads(threads)
    }
    /// Returns the maximum number of sockets the context may create.
    pub fn max_sockets(&self) -> Result<i32, ConfigError> {
        self.context.max_sockets()
    }
    /// Sets the maximum number of sockets the context may create.
    pub fn set_max_sockets(&self, max: i32) -> Result<(), ConfigError> {
        self.context.set_max_sockets(max)
    }
    /// Returns the largest value [`set_max_sockets`](Self::set_max_sockets) may
    /// take on this build.
    pub fn socket_limit(&self) -> Result<i32, ConfigError> {
        self.context.socket_limit()
    }
    /// Returns the OS scheduling priority of the context's I/O threads.
    pub fn thread_priority(&self) -> Result<i32, ConfigError> {
        self.context.thread_priority()
    }
    /// Sets the OS scheduling priority of the context's I/O threads.
    pub fn set_thread_priority(&self, priority: i32) -> Result<(), ConfigError> {
        self.context.set_thread_priority(priority)
    }
    /// Returns the OS scheduling policy of the context's I/O threads.
    pub fn thread_scheduling_policy(&self) -> Result<i32, ConfigError> {
        self.context.thread_scheduling_policy()
    }
    /// Sets the OS scheduling policy of the context's I/O threads.
    pub fn set_thread_scheduling_policy(&self, policy: i32) -> Result<(), ConfigError> {
        self.context.set_thread_scheduling_policy(policy)
    }
    /// Returns the default maximum inbound message size, in bytes, for new
    /// sockets.
    pub fn max_message_size(&self) -> Result<i32, ConfigError> {
        self.context.max_message_size()
    }
    /// Sets the default maximum inbound message size, in bytes, for new sockets.
    pub fn set_max_message_size(&self, size: i32) -> Result<(), ConfigError> {
        self.context.set_max_message_size(size)
    }
    /// Returns the size of the context's message worker thread pool.
    pub fn msg_t_size(&self) -> Result<i32, ConfigError> {
        self.context.msg_t_size()
    }
    /// Returns whether the context blocks on termination until queued messages
    /// have been sent.
    pub fn blocky(&self) -> Result<bool, ConfigError> {
        self.context.blocky()
    }
    /// Sets whether the context blocks on termination until queued messages have
    /// been sent.
    pub fn set_blocky(&self, blocky: bool) -> Result<(), ConfigError> {
        self.context.set_blocky(blocky)
    }
    /// Returns the prefix applied to the names of threads the context creates.
    pub fn thread_name_prefix(&self) -> Result<String, ConfigError> {
        self.context.thread_name_prefix()
    }
    /// Sets the prefix applied to the names of threads the context creates.
    pub fn set_thread_name_prefix(&self, prefix: &str) -> Result<(), ConfigError> {
        self.context.set_thread_name_prefix(prefix)
    }
    /// Returns whether high-water marks are sized automatically.
    pub fn auto_hwm_enabled(&self) -> Result<bool, ConfigError> {
        self.context.auto_hwm_enabled()
    }
    /// Sets whether high-water marks are sized automatically.
    pub fn set_auto_hwm_enabled(&self, enabled: bool) -> Result<(), ConfigError> {
        self.context.set_auto_hwm_enabled(enabled)
    }
    /// Returns the minimum delay between automatic high-water-mark
    /// recalculations.
    pub fn auto_hwm_recalc_debounce(&self) -> Result<Duration, ConfigError> {
        self.context.auto_hwm_recalc_debounce()
    }
    /// Sets the minimum delay between automatic high-water-mark recalculations.
    pub fn set_auto_hwm_recalc_debounce(&self, value: Duration) -> Result<(), ConfigError> {
        self.context.set_auto_hwm_recalc_debounce(value)
    }
    /// Returns the profile used to size high-water marks automatically.
    pub fn core_hwm_profile(&self) -> Result<AutoHwmProfile, ConfigError> {
        self.context.core_hwm_profile()
    }
    /// Sets the profile used to size high-water marks automatically; see
    /// [`AutoHwmProfile`].
    pub fn set_core_hwm_profile(&self, profile: AutoHwmProfile) -> Result<(), ConfigError> {
        self.context.set_core_hwm_profile(profile)
    }

    /// Returns the explicit context memory limit used by Core HWM planning.
    pub fn core_hwm_memory_limit_bytes(&self) -> Result<u64, ConfigError> {
        self.context.core_hwm_memory_limit_bytes()
    }

    /// Sets the explicit context memory limit used by Core HWM planning.
    pub fn set_core_hwm_memory_limit_bytes(&self, bytes: u64) -> Result<(), ConfigError> {
        self.context.set_core_hwm_memory_limit_bytes(bytes)
    }

    /// Returns the manually configured context-wide Core messaging budget.
    pub fn core_hwm_budget_bytes(&self) -> Result<u64, ConfigError> {
        self.context.core_hwm_budget_bytes()
    }

    /// Sets the context-wide Core messaging budget without applying a profile
    /// ratio in the binding.
    pub fn set_core_hwm_budget_bytes(&self, bytes: u64) -> Result<(), ConfigError> {
        self.context.set_core_hwm_budget_bytes(bytes)
    }
    /// Pins the context's I/O threads to also run on CPU core `cpu`.
    pub fn add_thread_affinity(&self, cpu: i32) -> Result<(), ConfigError> {
        self.context.add_thread_affinity(cpu)
    }
    /// Removes CPU core `cpu` from the context's I/O thread affinity.
    pub fn remove_thread_affinity(&self, cpu: i32) -> Result<(), ConfigError> {
        self.context.remove_thread_affinity(cpu)
    }
}
