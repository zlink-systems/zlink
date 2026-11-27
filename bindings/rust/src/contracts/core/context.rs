use crate::internal::ContextStorage;

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

/// Typed facade over context options.
pub struct ContextOptions<'a> {
    pub(crate) context: &'a ContextStorage,
}
