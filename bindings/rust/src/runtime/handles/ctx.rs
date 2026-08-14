use std::ffi::{CStr, CString, c_void};
use std::time::Duration;

use crate::core_context::{AutoHwmProfile, AutoHwmRecalcReason, Context, CoreHwmBudgetSnapshot};
use crate::error::{CloseError, ConfigError, ConfigResult};
use crate::ffi;
use crate::internal::ContextStorage;
use crate::native_errors::{check_close_rc, check_config_rc, config_validation_error, last_errno};
use crate::socket_contracts::{
    DealerSocket, PairSocket, PubSocket, RouterSocket, StreamSocket, SubSocket, XPubSocket,
    XSubSocket,
};

impl AutoHwmProfile {
    pub(crate) fn to_raw(self) -> i32 {
        match self {
            AutoHwmProfile::Compact => {
                ffi::zlink_auto_hwm_profile_t::ZLINK_AUTO_HWM_PROFILE_COMPACT as i32
            }
            AutoHwmProfile::LowLatency => {
                ffi::zlink_auto_hwm_profile_t::ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY as i32
            }
            AutoHwmProfile::Balanced => {
                ffi::zlink_auto_hwm_profile_t::ZLINK_AUTO_HWM_PROFILE_BALANCED as i32
            }
            AutoHwmProfile::Throughput => {
                ffi::zlink_auto_hwm_profile_t::ZLINK_AUTO_HWM_PROFILE_THROUGHPUT as i32
            }
        }
    }

    pub(crate) fn from_raw(raw: i32) -> Result<Self, ConfigError> {
        match raw {
            x if x == ffi::zlink_auto_hwm_profile_t::ZLINK_AUTO_HWM_PROFILE_COMPACT as i32 => {
                Ok(AutoHwmProfile::Compact)
            }
            x if x == ffi::zlink_auto_hwm_profile_t::ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY as i32 => {
                Ok(AutoHwmProfile::LowLatency)
            }
            x if x == ffi::zlink_auto_hwm_profile_t::ZLINK_AUTO_HWM_PROFILE_BALANCED as i32 => {
                Ok(AutoHwmProfile::Balanced)
            }
            x if x == ffi::zlink_auto_hwm_profile_t::ZLINK_AUTO_HWM_PROFILE_THROUGHPUT as i32 => {
                Ok(AutoHwmProfile::Throughput)
            }
            _ => Err(config_validation_error()),
        }
    }
}

impl AutoHwmRecalcReason {
    pub(crate) fn from_raw(raw: u32) -> Self {
        match raw {
            1 => AutoHwmRecalcReason::Initial,
            2 => AutoHwmRecalcReason::RoleChange,
            3 => AutoHwmRecalcReason::PolicyToggle,
            4 => AutoHwmRecalcReason::Refresh,
            5 => AutoHwmRecalcReason::DeferredShrink,
            _ => AutoHwmRecalcReason::None,
        }
    }
}

pub(crate) fn context_new() -> Result<Context, ConfigError> {
    let handle = unsafe { ffi::zlink_ctx_new() };
    if handle.is_null() {
        return Err(ConfigError::new(
            crate::error::ConfigResult::InvalidHandle,
            last_errno(),
        ));
    }
    Ok(Context {
        inner: Box::new(ContextStorage {
            handle,
            thread_name_prefix: std::sync::Mutex::new(String::new()),
        }),
    })
}

pub(crate) fn context_pair_socket(context: &Context) -> Result<PairSocket, ConfigError> {
    PairSocket::new(context)
}

pub(crate) fn context_pub_socket(context: &Context) -> Result<PubSocket, ConfigError> {
    PubSocket::new(context)
}

pub(crate) fn context_sub_socket(context: &Context) -> Result<SubSocket, ConfigError> {
    SubSocket::new(context)
}

pub(crate) fn context_dealer_socket(context: &Context) -> Result<DealerSocket, ConfigError> {
    DealerSocket::new(context)
}

pub(crate) fn context_router_socket(context: &Context) -> Result<RouterSocket, ConfigError> {
    RouterSocket::new(context)
}

pub(crate) fn context_xpub_socket(context: &Context) -> Result<XPubSocket, ConfigError> {
    XPubSocket::new(context)
}

pub(crate) fn context_xsub_socket(context: &Context) -> Result<XSubSocket, ConfigError> {
    XSubSocket::new(context)
}

pub(crate) fn context_stream_socket(context: &Context) -> Result<StreamSocket, ConfigError> {
    StreamSocket::new(context)
}

impl ContextStorage {
    fn set_int_option(&self, option: i32, value: i32) -> Result<(), ConfigError> {
        check_config_rc(unsafe { ffi::zlink_ctx_set(self.handle, raw_option(option), value) })
    }

    fn set_data_option(
        &self,
        option: i32,
        data: *const c_void,
        len: usize,
    ) -> Result<(), ConfigError> {
        check_config_rc(unsafe {
            ffi::zlink_ctx_set_data(self.handle, raw_option(option), data, len)
        })
    }

    fn set_u64_data_option(&self, option: i32, value: u64) -> Result<(), ConfigError> {
        self.set_data_option(
            option,
            (&value as *const u64).cast(),
            std::mem::size_of::<u64>(),
        )
    }

    fn get_int_option(&self, option: i32) -> Result<i32, ConfigError> {
        let mut result = ffi::zlink_config_result_t::ZLINK_CONFIG_OK;
        let value = unsafe { ffi::zlink_ctx_get(self.handle, raw_option(option), &mut result) };
        if result != ffi::zlink_config_result_t::ZLINK_CONFIG_OK {
            let code = match result {
                ffi::zlink_config_result_t::ZLINK_CONFIG_INVALID_HANDLE => {
                    ConfigResult::InvalidHandle
                }
                ffi::zlink_config_result_t::ZLINK_CONFIG_NOT_SUPPORTED => {
                    ConfigResult::NotSupported
                }
                _ => ConfigResult::InvalidArgument,
            };
            Err(ConfigError::new(code, last_errno()))
        } else {
            Ok(value)
        }
    }

    fn get_u64_data_option(&self, option: i32) -> Result<u64, ConfigError> {
        let mut value = 0_u64;
        let mut len = std::mem::size_of::<u64>();
        check_config_rc(unsafe {
            ffi::zlink_ctx_get_data(
                self.handle,
                raw_option(option),
                (&mut value as *mut u64).cast(),
                &mut len,
            )
        })?;
        if len != std::mem::size_of::<u64>() {
            return Err(config_validation_error());
        }
        Ok(value)
    }
}

impl ContextStorage {
    pub(crate) fn shutdown(&self) -> Result<(), CloseError> {
        check_close_rc(unsafe { ffi::zlink_ctx_shutdown(self.handle) })
    }

    pub(crate) fn recalculate_auto_hwm(&self) -> Result<(), ConfigError> {
        check_config_rc(unsafe { ffi::zlink_ctx_auto_hwm_recalculate(self.handle) })
    }

    pub(crate) fn core_hwm_budget_snapshot(&self) -> Result<CoreHwmBudgetSnapshot, ConfigError> {
        let mut native = ffi::zlink_auto_hwm_budget_snapshot_t::default();
        check_config_rc(unsafe {
            ffi::zlink_ctx_get_auto_hwm_budget_snapshot(self.handle, &mut native)
        })?;
        Ok(CoreHwmBudgetSnapshot {
            abi_version: native.abi_version,
            struct_size: native.struct_size,
            budget_generation: native.budget_generation,
            measurement_epoch: native.measurement_epoch,
            configured_memory_limit_bytes: native.configured_memory_limit_bytes,
            runtime_memory_limit_bytes: native.runtime_memory_limit_bytes,
            resolved_memory_limit_bytes: native.resolved_memory_limit_bytes,
            configured_core_budget_bytes: native.configured_core_budget_bytes,
            effective_core_budget_bytes: native.effective_core_budget_bytes,
            total_planned_hwm_bytes: native.total_planned_hwm_bytes,
            total_applied_hwm_bytes: native.total_applied_hwm_bytes,
            manual_reserved_hwm_bytes: native.manual_reserved_hwm_bytes,
            core_queue_accounted_bytes: native.core_queue_accounted_bytes,
            application_accounted_bytes: native.application_accounted_bytes,
            current_accounted_bytes: native.current_accounted_bytes,
            provisional_accounted_bytes: native.provisional_accounted_bytes,
            peak_accounted_bytes: native.peak_accounted_bytes,
            completion_current_accounted_bytes: native.completion_current_accounted_bytes,
            completion_peak_accounted_bytes: native.completion_peak_accounted_bytes,
            completion_pending_message_count: native.completion_pending_message_count,
            total_messaging_accounted_bytes: native.total_messaging_accounted_bytes,
            monitor_queue_applied_hwm_bytes: native.monitor_queue_applied_hwm_bytes,
            monitor_queue_accounted_bytes: native.monitor_queue_accounted_bytes,
            total_instance_applied_hwm_bytes: native.total_instance_applied_hwm_bytes,
            total_instance_accounted_bytes: native.total_instance_accounted_bytes,
            oversize_admission_count: native.oversize_admission_count,
            largest_oversize_message_bytes: native.largest_oversize_message_bytes,
            active_directional_queue_count: native.active_directional_queue_count,
            active_completion_directional_queue_count: native
                .active_completion_directional_queue_count,
            active_send_queue_count: native.active_send_queue_count,
            active_receive_queue_count: native.active_receive_queue_count,
            outstanding_application_lease_count: native.outstanding_application_lease_count,
            retired_queue_count: native.retired_queue_count,
            deferred_origin_credit_bytes: native.deferred_origin_credit_bytes,
            unlimited_manual_queue_count: native.unlimited_manual_queue_count,
            blocked_ratio_ppm: native.blocked_ratio_ppm,
            flags: native.flags,
            reserved_u64: native.reserved_u64,
        })
    }

    pub(crate) fn reset_core_hwm_budget_metrics(&self) -> Result<(), ConfigError> {
        check_config_rc(unsafe { ffi::zlink_ctx_reset_auto_hwm_budget_metrics(self.handle) })
    }
}

impl ContextStorage {
    pub(crate) fn io_threads(&self) -> Result<i32, ConfigError> {
        self.get_int_option(ffi::zlink_ctx_option_t::ZLINK_IO_THREADS as i32)
    }

    pub(crate) fn set_io_threads(&self, threads: i32) -> Result<(), ConfigError> {
        self.set_int_option(ffi::zlink_ctx_option_t::ZLINK_IO_THREADS as i32, threads)
    }

    pub(crate) fn max_sockets(&self) -> Result<i32, ConfigError> {
        self.get_int_option(ffi::zlink_ctx_option_t::ZLINK_MAX_SOCKETS as i32)
    }

    pub(crate) fn set_max_sockets(&self, max: i32) -> Result<(), ConfigError> {
        self.set_int_option(ffi::zlink_ctx_option_t::ZLINK_MAX_SOCKETS as i32, max)
    }

    pub(crate) fn socket_limit(&self) -> Result<i32, ConfigError> {
        self.get_int_option(ffi::zlink_ctx_option_t::ZLINK_SOCKET_LIMIT as i32)
    }

    pub(crate) fn thread_priority(&self) -> Result<i32, ConfigError> {
        self.get_int_option(3)
    }

    pub(crate) fn set_thread_priority(&self, priority: i32) -> Result<(), ConfigError> {
        self.set_int_option(3, priority)
    }

    pub(crate) fn thread_scheduling_policy(&self) -> Result<i32, ConfigError> {
        self.get_int_option(ffi::zlink_ctx_option_t::ZLINK_THREAD_SCHED_POLICY as i32)
    }

    pub(crate) fn set_thread_scheduling_policy(&self, policy: i32) -> Result<(), ConfigError> {
        self.set_int_option(
            ffi::zlink_ctx_option_t::ZLINK_THREAD_SCHED_POLICY as i32,
            policy,
        )
    }

    pub(crate) fn max_message_size(&self) -> Result<i32, ConfigError> {
        self.get_int_option(ffi::zlink_ctx_option_t::ZLINK_MAX_MSGSZ as i32)
    }

    pub(crate) fn set_max_message_size(&self, size: i32) -> Result<(), ConfigError> {
        self.set_int_option(ffi::zlink_ctx_option_t::ZLINK_MAX_MSGSZ as i32, size)
    }

    pub(crate) fn msg_t_size(&self) -> Result<i32, ConfigError> {
        self.get_int_option(ffi::zlink_ctx_option_t::ZLINK_MSG_T_SIZE as i32)
    }

    pub(crate) fn blocky(&self) -> Result<bool, ConfigError> {
        Ok(self.get_int_option(ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_BLOCKY as i32)? != 0)
    }

    pub(crate) fn set_blocky(&self, blocky: bool) -> Result<(), ConfigError> {
        self.set_int_option(
            ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_BLOCKY as i32,
            if blocky { 1 } else { 0 },
        )
    }

    /// Get the thread name prefix (returned from the cached value set via `set_thread_name_prefix`).
    pub(crate) fn thread_name_prefix(&self) -> Result<String, ConfigError> {
        Ok(self
            .thread_name_prefix
            .lock()
            .map_err(|_| config_validation_error())?
            .clone())
    }

    /// Set the thread name prefix (applied to I/O threads created by this context).
    pub(crate) fn set_thread_name_prefix(&self, prefix: &str) -> Result<(), ConfigError> {
        let c = CString::new(prefix).map_err(|_| config_validation_error())?;
        let bytes = c.as_bytes(); // excludes NUL
        self.set_data_option(
            ffi::zlink_ctx_option_t::ZLINK_THREAD_NAME_PREFIX as i32,
            bytes.as_ptr().cast(),
            bytes.len(),
        )?;
        *self
            .thread_name_prefix
            .lock()
            .map_err(|_| config_validation_error())? = prefix.to_owned();
        Ok(())
    }

    /// Get whether auto HWM is enabled.
    pub(crate) fn auto_hwm_enabled(&self) -> Result<bool, ConfigError> {
        Ok(
            self.get_int_option(ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_ENABLE as i32)?
                != 0,
        )
    }

    /// Enable or disable auto HWM.
    pub(crate) fn set_auto_hwm_enabled(&self, enabled: bool) -> Result<(), ConfigError> {
        self.set_int_option(
            ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_ENABLE as i32,
            if enabled { 1 } else { 0 },
        )
    }

    /// Get the auto HWM recalculation debounce interval.
    pub(crate) fn auto_hwm_recalc_debounce(&self) -> Result<Duration, ConfigError> {
        let ms = self.get_int_option(
            ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS as i32,
        )?;
        Ok(Duration::from_millis(ms.max(0) as u64))
    }

    /// Set the auto HWM recalculation debounce interval.
    pub(crate) fn set_auto_hwm_recalc_debounce(&self, value: Duration) -> Result<(), ConfigError> {
        let ms = crate::ctx::duration_to_millis(value)?;
        self.set_int_option(
            ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_RECALC_DEBOUNCE_MS as i32,
            ms,
        )
    }

    pub(crate) fn core_hwm_profile(&self) -> Result<AutoHwmProfile, ConfigError> {
        AutoHwmProfile::from_raw(
            self.get_int_option(ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_PROFILE as i32)?,
        )
    }

    pub(crate) fn set_core_hwm_profile(&self, profile: AutoHwmProfile) -> Result<(), ConfigError> {
        self.set_int_option(
            ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_PROFILE as i32,
            profile.to_raw(),
        )
    }

    pub(crate) fn core_hwm_memory_limit_bytes(&self) -> Result<u64, ConfigError> {
        self.get_u64_data_option(
            ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES as i32,
        )
    }

    pub(crate) fn set_core_hwm_memory_limit_bytes(&self, bytes: u64) -> Result<(), ConfigError> {
        self.set_u64_data_option(
            ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_MEMORY_LIMIT_BYTES as i32,
            bytes,
        )
    }

    pub(crate) fn core_hwm_budget_bytes(&self) -> Result<u64, ConfigError> {
        self.get_u64_data_option(
            ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES as i32,
        )
    }

    pub(crate) fn set_core_hwm_budget_bytes(&self, bytes: u64) -> Result<(), ConfigError> {
        self.set_u64_data_option(
            ffi::zlink_ctx_option_t::ZLINK_CTX_OPT_AUTO_HWM_CORE_BUDGET_BYTES as i32,
            bytes,
        )
    }

    pub(crate) fn add_thread_affinity(&self, cpu: i32) -> Result<(), ConfigError> {
        self.set_int_option(
            ffi::zlink_ctx_option_t::ZLINK_THREAD_AFFINITY_CPU_ADD as i32,
            cpu,
        )
    }

    pub(crate) fn remove_thread_affinity(&self, cpu: i32) -> Result<(), ConfigError> {
        self.set_int_option(
            ffi::zlink_ctx_option_t::ZLINK_THREAD_AFFINITY_CPU_REMOVE as i32,
            cpu,
        )
    }
}

fn raw_option(value: i32) -> ffi::zlink_ctx_option_t {
    unsafe { std::mem::transmute(value) }
}

impl Drop for ContextStorage {
    fn drop(&mut self) {
        unsafe {
            ffi::zlink_ctx_term(self.handle);
        }
    }
}

// ---------------------------------------------------------------------------
// Version helper
// ---------------------------------------------------------------------------

/// Return the runtime library version as `(major, minor, patch)`.
pub fn version() -> (i32, i32, i32) {
    let (mut major, mut minor, mut patch) = (0i32, 0i32, 0i32);
    unsafe {
        ffi::zlink_version(&mut major, &mut minor, &mut patch);
    }
    (major, minor, patch)
}

/// Check whether the library supports a given capability (e.g. `"ipc"`, `"tls"`).
pub fn has(capability: &str) -> bool {
    let c = std::ffi::CString::new(capability).unwrap_or_default();
    unsafe { ffi::zlink_has(c.as_ptr()) != 0 }
}

/// Returns the native error text for an errno value.
pub fn strerror(errnum: i32) -> &'static str {
    let ptr = unsafe { ffi::zlink_strerror(errnum) };
    if ptr.is_null() {
        ""
    } else {
        unsafe { CStr::from_ptr(ptr) }.to_str().unwrap_or("")
    }
}

// ---------------------------------------------------------------------------
// Duration → millis helper (with overflow check per Boundary Cost Policy)
// ---------------------------------------------------------------------------

pub(crate) fn duration_to_millis(d: Duration) -> Result<i32, ConfigError> {
    let ms = d.as_millis();
    if ms > i32::MAX as u128 {
        return Err(config_validation_error());
    }
    Ok(ms as i32)
}

pub(crate) fn context_handle(ctx: &Context) -> *mut c_void {
    ctx.inner.handle
}
