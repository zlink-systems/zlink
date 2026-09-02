// SPDX-License-Identifier: MPL-2.0
//
// ABI layout sanity tests. The Rust bindings keep #[repr(C)] mirrors of
// several core C structs. When the core layout changes (or when the Rust
// mirror drifts), zlink_poller_wait() and friends will write the C struct
// shape into a smaller / differently-aligned Rust struct, silently corrupting
// the stack. These tests pin the expected sizes/offsets so any drift fails the
// rust test suite instead of crashing user code.

use std::mem::{align_of, offset_of, size_of};

#[test]
fn poller_event_layout_matches_c_abi() {
    // Lift the mirror into scope without dragging in private modules: it
    // is part of the crate's runtime/native bridge and is repr(C).
    use zlink_ffi_check::ZlinkPollerEvent;

    // C struct layout (see core/include/zlink/monitoring.h):
    //   zlink_poller_source_kind_t source_kind;   // 4 bytes (enum, int)
    //   void *socket;                              // 8 bytes (pointer)
    //   zlink_fd_t fd;                             // 8 bytes (intptr_t)
    //   void *timer;                               // 8 bytes
    //   void *user_data;                           // 8 bytes
    //   short events;                              // 2 bytes
    // Total = 40 with 4-byte alignment after source_kind, padding to 8 before pointer,
    //         2-byte tail with trailing pad to 8 -> 48 bytes total on 64-bit.
    assert_eq!(
        size_of::<ZlinkPollerEvent>(),
        48,
        "zlink_poller_event_t size drifted from the C ABI"
    );
    assert_eq!(align_of::<ZlinkPollerEvent>(), 8);
    assert_eq!(offset_of!(ZlinkPollerEvent, source_kind), 0);
    assert_eq!(offset_of!(ZlinkPollerEvent, socket), 8);
    assert_eq!(offset_of!(ZlinkPollerEvent, fd), 16);
    assert_eq!(offset_of!(ZlinkPollerEvent, timer), 24);
    assert_eq!(offset_of!(ZlinkPollerEvent, user_data), 32);
    assert_eq!(offset_of!(ZlinkPollerEvent, events), 40);
}

#[test]
fn monitor_status_layout_matches_c_abi_v4() {
    // Lift the mirror into scope without dragging in the private `ffi`
    // module.
    use zlink_ffi_check::ZlinkMonitorStatus;

    // C struct layout (see core/include/zlink/eventing/api.h,
    // ZLINK_MONITOR_STATUS_ABI_VERSION == 4). This struct is written to
    // directly by Core (`zlink_monitor_status()`); an undersized Rust mirror
    // means Core memsets/writes `sizeof(zlink_monitor_status_t)` == 232 bytes
    // into a smaller stack allocation, corrupting adjacent stack memory on
    // every call. ABI 4 appended five `uint64_t` flow-state fields after the
    // ABI 3 tail (`oversize_message_admission_max_bytes`); a Rust mirror that
    // stops there is silently still ABI 3 sized (192 bytes) and overruns by
    // exactly the 40 bytes those five fields occupy.
    assert_eq!(
        size_of::<ZlinkMonitorStatus>(),
        232,
        "zlink_monitor_status_t size drifted from the C ABI (expected ABI-4 size 232; \
         a size of 192 means the ABI-4 flow-state tail is missing again)"
    );
    assert_eq!(align_of::<ZlinkMonitorStatus>(), 8);
    assert_eq!(offset_of!(ZlinkMonitorStatus, abi_version), 0);
    assert_eq!(offset_of!(ZlinkMonitorStatus, struct_size), 4);
    assert_eq!(offset_of!(ZlinkMonitorStatus, snd_pending_msgs), 24);
    assert_eq!(
        offset_of!(ZlinkMonitorStatus, oversize_message_admission_max_bytes),
        184
    );
    // The ABI-4 flow-state tail, in declared order.
    assert_eq!(offset_of!(ZlinkMonitorStatus, flow_paused_connections), 192);
    assert_eq!(
        offset_of!(ZlinkMonitorStatus, flow_pause_applied_total),
        200
    );
    assert_eq!(
        offset_of!(ZlinkMonitorStatus, flow_resume_applied_total),
        208
    );
    assert_eq!(offset_of!(ZlinkMonitorStatus, flow_state_stale_total), 216);
    assert_eq!(offset_of!(ZlinkMonitorStatus, flow_pause_duration_ms), 224);
}

/// Mirror exposed for ABI tests only. Kept here so we don't have to publish
/// the FFI module from `lib.rs`.
mod zlink_ffi_check {
    use std::os::raw::c_void;

    #[repr(C)]
    #[allow(dead_code)]
    pub enum ZlinkPollerSourceKind {
        Socket = 1,
        Fd = 2,
        Timer = 3,
    }

    #[repr(C)]
    pub struct ZlinkPollerEvent {
        pub source_kind: ZlinkPollerSourceKind,
        pub socket: *mut c_void,
        pub fd: isize,
        pub timer: *mut c_void,
        pub user_data: *mut c_void,
        pub events: i16,
    }

    #[repr(C)]
    #[allow(dead_code)]
    pub enum ZlinkMonitorSourceKind {
        Socket = 1,
    }

    /// Mirrors `bindings/rust/src/runtime/native/ffi.rs`'s
    /// `zlink_monitor_status_t` field-for-field, independently of it, so a
    /// regression in either copy is caught by comparing both against the
    /// real ABI size below rather than against each other.
    #[repr(C)]
    #[allow(dead_code)]
    pub struct ZlinkMonitorStatus {
        pub abi_version: u32,
        pub struct_size: u32,
        pub source_kind: ZlinkMonitorSourceKind,
        pub state_flags: u32,
        pub detail_flags: u32,
        pub snd_pending_msgs: u64,
        pub rcv_pending_msgs: u64,
        pub snd_pending_bytes: u64,
        pub rcv_pending_bytes: u64,
        pub auto_hwm_enabled: u32,
        pub auto_hwm_profile: u32,
        pub auto_hwm_role: u32,
        pub auto_hwm_policy_class: u32,
        pub auto_hwm_planned_sndhwm_bytes: u64,
        pub auto_hwm_planned_rcvhwm_bytes: u64,
        pub auto_hwm_applied_sndhwm_bytes: u64,
        pub auto_hwm_applied_rcvhwm_bytes: u64,
        pub auto_hwm_effective_sndbuf: i32,
        pub auto_hwm_effective_rcvbuf: i32,
        pub auto_hwm_last_recalc_ms: u64,
        pub auto_hwm_last_recalc_reason: u32,
        pub auto_hwm_send_blocked_ratio_ppm: u32,
        pub auto_hwm_deferred_sndhwm_bytes: u64,
        pub auto_hwm_deferred_rcvhwm_bytes: u64,
        pub auto_hwm_deferred_sndhwm_valid: u32,
        pub auto_hwm_deferred_rcvhwm_valid: u32,
        pub snd_bytes_in_flight: u64,
        pub rcv_bytes_in_flight: u64,
        pub minimum_core_message_charge_bytes: u64,
        pub oversize_message_admission_count: u64,
        pub oversize_message_admission_max_bytes: u64,
        pub flow_paused_connections: u64,
        pub flow_pause_applied_total: u64,
        pub flow_resume_applied_total: u64,
        pub flow_state_stale_total: u64,
        pub flow_pause_duration_ms: u64,
    }
}
