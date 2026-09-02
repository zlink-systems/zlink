use std::ffi::{CStr, c_void};
use std::mem::MaybeUninit;

use crate::core_context::AutoHwmRecalcReason;
use crate::error::{CloseError, ConfigError, RecvError};
use crate::ffi;
use crate::internal::MonitorStorage;
use crate::message::RoutingId;
use crate::monitor_contracts::{
    MonitorEvent, MonitorEventFlags, MonitorEventType, MonitorSourceKind, MonitorStatus,
    Monitorable, SocketMonitor, SocketMonitorOpenOptions,
};
use crate::native_errors::{check_close_rc, check_config_rc, check_recv_rc, last_errno};

// ---------------------------------------------------------------------------
// MonitorEvent – typed socket monitor event
// ---------------------------------------------------------------------------

impl MonitorEvent {
    fn from_raw(raw: &ffi::zlink_monitor_event_t) -> Self {
        Self {
            event: MonitorEventType(raw.event),
            value: raw.value,
            routing_id: RoutingId::from_raw_optional(raw.routing_id),
            local_addr: cstr_array_to_string(&raw.local_addr),
            remote_addr: cstr_array_to_string(&raw.remote_addr),
            connection_id: raw.connection_id,
            transport_lane: raw.transport_lane,
            flags: MonitorEventFlags::from_raw(raw.flags),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn monitor_event_from_raw_projects_the_full_payload_without_truncation() {
        // The native `value` field is a u64 (core/include/zlink/eventing/api.h);
        // the public MonitorEvent used to narrow it to u32, silently dropping
        // the high bits. Pick a value that only round-trips correctly if the
        // full 64 bits survive the conversion.
        let large_value: u64 = u64::from(u32::MAX) + 1_000_000;

        let raw = ffi::zlink_monitor_event_t {
            event: 0x1000,
            value: large_value,
            routing_id: ffi::zlink_routing_id_t {
                size: 0,
                data: [0; 255],
            },
            local_addr: [0; 256],
            remote_addr: [0; 256],
            connection_id: 0xAAAA_BBBB_CCCC_DDDD,
            transport_lane: 3,
            flags: MonitorEventFlags::CONNECTION_READY_EDGE.bits()
                | MonitorEventFlags::SEND_FLOW_WRITABLE.bits(),
        };

        let event = MonitorEvent::from_raw(&raw);

        assert_eq!(event.value, large_value);
        assert!(event.value > u64::from(u32::MAX));
        assert_eq!(event.connection_id, 0xAAAA_BBBB_CCCC_DDDD);
        assert_eq!(event.transport_lane, 3);
        assert!(
            event
                .flags
                .contains(MonitorEventFlags::CONNECTION_READY_EDGE)
        );
        assert!(event.flags.contains(MonitorEventFlags::SEND_FLOW_WRITABLE));
        assert!(
            !event
                .flags
                .contains(MonitorEventFlags::FLOW_STATE_STALE_EPOCH)
        );
    }
}

impl MonitorStatus {
    pub(crate) fn from_raw(raw: &ffi::zlink_monitor_status_t) -> Self {
        Self {
            source_kind: match raw.source_kind {
                ffi::zlink_monitor_source_kind_t::ZLINK_MONITOR_SOURCE_SOCKET => {
                    MonitorSourceKind::Socket
                }
            },
            abi_version: raw.abi_version,
            struct_size: raw.struct_size,
            state_flags: raw.state_flags,
            detail_flags: raw.detail_flags,
            snd_pending_msgs: raw.snd_pending_msgs,
            rcv_pending_msgs: raw.rcv_pending_msgs,
            snd_pending_bytes: raw.snd_pending_bytes,
            rcv_pending_bytes: raw.rcv_pending_bytes,
            auto_hwm_enabled: raw.auto_hwm_enabled != 0,
            auto_hwm_profile: raw.auto_hwm_profile,
            auto_hwm_role: raw.auto_hwm_role,
            auto_hwm_policy_class: raw.auto_hwm_policy_class,
            auto_hwm_planned_sndhwm_bytes: raw.auto_hwm_planned_sndhwm_bytes,
            auto_hwm_planned_rcvhwm_bytes: raw.auto_hwm_planned_rcvhwm_bytes,
            auto_hwm_applied_sndhwm_bytes: raw.auto_hwm_applied_sndhwm_bytes,
            auto_hwm_applied_rcvhwm_bytes: raw.auto_hwm_applied_rcvhwm_bytes,
            auto_hwm_effective_sndbuf: raw.auto_hwm_effective_sndbuf,
            auto_hwm_effective_rcvbuf: raw.auto_hwm_effective_rcvbuf,
            auto_hwm_last_recalc_ms: raw.auto_hwm_last_recalc_ms,
            auto_hwm_last_recalc_reason: AutoHwmRecalcReason::from_raw(
                raw.auto_hwm_last_recalc_reason,
            ),
            auto_hwm_send_blocked_ratio_ppm: raw.auto_hwm_send_blocked_ratio_ppm,
            auto_hwm_deferred_sndhwm_bytes: raw.auto_hwm_deferred_sndhwm_bytes,
            auto_hwm_deferred_rcvhwm_bytes: raw.auto_hwm_deferred_rcvhwm_bytes,
            auto_hwm_deferred_sndhwm_valid: raw.auto_hwm_deferred_sndhwm_valid != 0,
            auto_hwm_deferred_rcvhwm_valid: raw.auto_hwm_deferred_rcvhwm_valid != 0,
            snd_bytes_in_flight: raw.snd_bytes_in_flight,
            rcv_bytes_in_flight: raw.rcv_bytes_in_flight,
            minimum_core_message_charge_bytes: raw.minimum_core_message_charge_bytes,
            oversize_message_admission_count: raw.oversize_message_admission_count,
            oversize_message_admission_max_bytes: raw.oversize_message_admission_max_bytes,
            flow_paused_connections: raw.flow_paused_connections,
            flow_pause_applied_total: raw.flow_pause_applied_total,
            flow_resume_applied_total: raw.flow_resume_applied_total,
            flow_state_stale_total: raw.flow_state_stale_total,
            flow_pause_duration_ms: raw.flow_pause_duration_ms,
        }
    }
}

pub(crate) fn monitor_status_is_ready(status: &MonitorStatus) -> bool {
    status.state_flags & ffi::ZLINK_MONITOR_STATE_READY != 0
}

pub(crate) fn monitor_status_is_closed(status: &MonitorStatus) -> bool {
    status.state_flags & ffi::ZLINK_MONITOR_STATE_CLOSED != 0
}

pub(crate) fn monitor_status_has_flow_state_detail(status: &MonitorStatus) -> bool {
    status.detail_flags & ffi::ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE != 0
}

// ---------------------------------------------------------------------------
// SocketMonitor
// ---------------------------------------------------------------------------

pub(crate) fn socket_monitor_open(socket: &dyn Monitorable) -> Result<SocketMonitor, ConfigError> {
    socket_monitor_open_with_options(socket, SocketMonitorOpenOptions::default())
}

/// Open a socket monitor with explicit typed options.
pub(crate) fn socket_monitor_open_with_options(
    socket: &dyn Monitorable,
    options: SocketMonitorOpenOptions,
) -> Result<SocketMonitor, ConfigError> {
    let target = monitorable_handle(socket)?;
    let opts = ffi::zlink_socket_monitor_open_options_t {
        events: options.events.bits(),
        monitor_hwm_bytes: options.monitor_hwm_bytes,
    };
    let handle = unsafe { ffi::zlink_socket_monitor_open(target, &opts) };
    if handle.is_null() {
        return Err(crate::error::ConfigError::new(
            crate::error::ConfigResult::InvalidArgument,
            last_errno(),
        ));
    }
    Ok(SocketMonitor {
        inner: Box::new(MonitorStorage { handle }),
    })
}

impl MonitorStorage {
    /// Blocking receive of a monitor event.
    pub(crate) fn recv(&self) -> Result<MonitorEvent, RecvError> {
        self.recv_with_flags(crate::flags::RecvFlags::NONE)
            .and_then(|opt| {
                opt.ok_or_else(|| RecvError::new(crate::error::RecvResult::NoData, libc::EAGAIN))
            })
    }

    /// Non-blocking receive of a monitor event. Returns `Ok(None)` when no event is available.
    pub(crate) fn recv_with_flags(
        &self,
        flags: crate::flags::RecvFlags,
    ) -> Result<Option<MonitorEvent>, RecvError> {
        use crate::error::RecvResult;
        let mut raw = MaybeUninit::<ffi::zlink_socket_monitor_event_t>::uninit();
        let rc =
            unsafe { ffi::zlink_socket_monitor_recv(self.handle, raw.as_mut_ptr(), flags.bits()) };
        if rc == RecvResult::NoData as i32 {
            return Ok(None);
        }
        check_recv_rc(rc)?;
        let val = unsafe { raw.assume_init() };
        Ok(Some(MonitorEvent::from_raw(&val)))
    }

    /// Read the current monitor status.
    pub(crate) fn status(&self) -> Result<MonitorStatus, ConfigError> {
        let mut raw = MaybeUninit::<ffi::zlink_monitor_status_t>::uninit();
        check_config_rc(unsafe { ffi::zlink_monitor_status(self.handle, raw.as_mut_ptr()) })?;
        let val = unsafe { raw.assume_init() };
        Ok(MonitorStatus::from_raw(&val))
    }

    pub(crate) fn close(&mut self) -> Result<(), CloseError> {
        if self.handle.is_null() {
            return Ok(());
        }
        let mut h = self.handle;
        check_close_rc(unsafe { ffi::zlink_monitor_close(&mut h) })?;
        self.handle = std::ptr::null_mut();
        Ok(())
    }
}

impl Drop for MonitorStorage {
    fn drop(&mut self) {
        if self.handle.is_null() {
            return;
        }
        let mut handle = self.handle;
        let rc = unsafe { ffi::zlink_monitor_close(&mut handle) };
        if rc != 0 {
            crate::internal::defer_native_close(
                crate::internal::DeferredCloseKind::Monitor,
                self.handle,
            );
        }
    }
}

fn monitorable_handle(source: &dyn Monitorable) -> Result<*mut c_void, ConfigError> {
    let any = source.as_any();
    if let Some(socket) = any.downcast_ref::<crate::PairSocket>() {
        return Ok(crate::socket::pair_handle(socket));
    }
    macro_rules! downcast_socket {
        ($ty:ident, $inner:path) => {
            if let Some(socket) = any.downcast_ref::<crate::$ty>() {
                return Ok($inner(socket).handle);
            }
        };
    }
    downcast_socket!(PubSocket, crate::socket::pub_inner);
    downcast_socket!(SubSocket, crate::socket::sub_inner);
    downcast_socket!(DealerSocket, crate::socket::dealer_inner);
    downcast_socket!(RouterSocket, crate::socket::router_inner);
    downcast_socket!(XPubSocket, crate::socket::xpub_inner);
    downcast_socket!(XSubSocket, crate::socket::xsub_inner);
    downcast_socket!(StreamSocket, crate::socket::stream_inner);
    Err(ConfigError::new(
        crate::error::ConfigResult::InvalidArgument,
        libc::EINVAL,
    ))
}

macro_rules! impl_monitorable {
    ($ty:ident) => {
        impl crate::monitor_contracts::private::Sealed for crate::$ty {}

        impl Monitorable for crate::$ty {
            fn as_any(&self) -> &dyn std::any::Any {
                self
            }
        }
    };
}

impl_monitorable!(PubSocket);
impl_monitorable!(SubSocket);
impl_monitorable!(DealerSocket);
impl_monitorable!(RouterSocket);
impl_monitorable!(XPubSocket);
impl_monitorable!(XSubSocket);
impl_monitorable!(StreamSocket);

impl crate::monitor_contracts::private::Sealed for crate::PairSocket {}

impl Monitorable for crate::PairSocket {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn cstr_array_to_string(buf: &[i8]) -> String {
    unsafe {
        let ptr = buf.as_ptr();
        if *ptr == 0 {
            String::new()
        } else {
            CStr::from_ptr(ptr).to_string_lossy().into_owned()
        }
    }
}
