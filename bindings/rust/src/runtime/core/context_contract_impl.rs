// SPDX-License-Identifier: MPL-2.0

use crate::internal::ContextStorage;
use crate::{
    AutoHwmProfile, CloseError, ConfigError, Context, ContextOptions, CoreHwmBudgetSnapshot,
    DealerSocket, PairSocket, PubSocket, RouterSocket, StreamSocket, SubSocket, XPubSocket,
    XSubSocket,
};
use std::time::Duration;

impl Context {
    /// Create a new context with default settings.
    pub fn new() -> Result<Self, ConfigError> {
        ContextStorage::new_context()
    }

    /// Create a PAIR socket.
    pub fn create_pair_socket(&self) -> Result<PairSocket, ConfigError> {
        PairSocket::new(self)
    }

    /// Use [`Self::create_pair_socket`] for new code.
    #[deprecated(note = "use create_pair_socket")]
    pub fn pair_socket(&self) -> Result<PairSocket, ConfigError> {
        self.create_pair_socket()
    }

    /// Create a PUB socket.
    pub fn create_pub_socket(&self) -> Result<PubSocket, ConfigError> {
        PubSocket::new(self)
    }

    /// Use [`Self::create_pub_socket`] for new code.
    #[deprecated(note = "use create_pub_socket")]
    pub fn pub_socket(&self) -> Result<PubSocket, ConfigError> {
        self.create_pub_socket()
    }

    /// Create a SUB socket.
    pub fn create_sub_socket(&self) -> Result<SubSocket, ConfigError> {
        SubSocket::new(self)
    }

    /// Use [`Self::create_sub_socket`] for new code.
    #[deprecated(note = "use create_sub_socket")]
    pub fn sub_socket(&self) -> Result<SubSocket, ConfigError> {
        self.create_sub_socket()
    }

    /// Create a DEALER socket.
    pub fn create_dealer_socket(&self) -> Result<DealerSocket, ConfigError> {
        DealerSocket::new(self)
    }

    /// Use [`Self::create_dealer_socket`] for new code.
    #[deprecated(note = "use create_dealer_socket")]
    pub fn dealer_socket(&self) -> Result<DealerSocket, ConfigError> {
        self.create_dealer_socket()
    }

    /// Create a ROUTER socket.
    pub fn create_router_socket(&self) -> Result<RouterSocket, ConfigError> {
        RouterSocket::new(self)
    }

    /// Use [`Self::create_router_socket`] for new code.
    #[deprecated(note = "use create_router_socket")]
    pub fn router_socket(&self) -> Result<RouterSocket, ConfigError> {
        self.create_router_socket()
    }

    /// Create an XPUB socket.
    pub fn create_xpub_socket(&self) -> Result<XPubSocket, ConfigError> {
        XPubSocket::new(self)
    }

    /// Use [`Self::create_xpub_socket`] for new code.
    #[deprecated(note = "use create_xpub_socket")]
    pub fn xpub_socket(&self) -> Result<XPubSocket, ConfigError> {
        self.create_xpub_socket()
    }

    /// Create an XSUB socket.
    pub fn create_xsub_socket(&self) -> Result<XSubSocket, ConfigError> {
        XSubSocket::new(self)
    }

    /// Use [`Self::create_xsub_socket`] for new code.
    #[deprecated(note = "use create_xsub_socket")]
    pub fn xsub_socket(&self) -> Result<XSubSocket, ConfigError> {
        self.create_xsub_socket()
    }

    /// Create a STREAM socket.
    pub fn create_stream_socket(&self) -> Result<StreamSocket, ConfigError> {
        StreamSocket::new(self)
    }

    /// Use [`Self::create_stream_socket`] for new code.
    #[deprecated(note = "use create_stream_socket")]
    pub fn stream_socket(&self) -> Result<StreamSocket, ConfigError> {
        self.create_stream_socket()
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
