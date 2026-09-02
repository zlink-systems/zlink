use std::any::Any;
use std::ops::{BitOr, BitOrAssign};

use crate::core_context::AutoHwmRecalcReason;
use crate::error::{CloseError, ConfigError, RecvError};
use crate::flags::RecvFlags;
use crate::internal::MonitorStorage;
use crate::routing_id::RoutingId;

pub(crate) mod private {
    pub(crate) trait Sealed {}
}

/// A built-in socket that a [`SocketMonitor`] can observe.
///
/// The trait is sealed because the binding must map a source to its native
/// socket handle; custom implementations cannot provide that mapping safely.
#[allow(private_bounds)]
pub trait Monitorable: Any + private::Sealed {
    /// Internal type identity used to map the built-in socket source to Core.
    #[doc(hidden)]
    fn as_any(&self) -> &dyn Any;
}

/// Typed bitmask for socket monitor subscriptions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SocketMonitorEventMask(u32);

impl SocketMonitorEventMask {
    /// Subscribes to every monitor event.
    pub const ALL: Self = Self(0x7FFFF);
    /// Subscribes only to the connection-ready event.
    pub const CONNECTION_READY: Self = Self(0x1000);
    /// Subscribes to the paired DEALER/ROUTER completion lane applying a
    /// remote PAUSED state to a connection for the first time
    /// (core-byte-hwm-flow-control-plan.ko.md §6). No event fires for a
    /// normal data frame.
    pub const SEND_FLOW_PAUSED: Self = Self(1 << 16);
    /// Subscribes to a remote RUNNING transition removing the last
    /// remote-pause cause for a connection.
    pub const SEND_FLOW_RESUMED: Self = Self(1 << 17);
    /// Subscribes to a stale or duplicate flow-state frame being ignored.
    pub const FLOW_STATE_STALE: Self = Self(1 << 18);

    /// Returns the raw mask bits.
    pub const fn bits(self) -> u32 {
        self.0
    }
}

impl Default for SocketMonitorEventMask {
    fn default() -> Self {
        Self::ALL
    }
}

impl BitOr for SocketMonitorEventMask {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl BitOrAssign for SocketMonitorEventMask {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

/// Convenience alias for [`SocketMonitorEventMask::ALL`].
pub const MONITOR_EVENT_ALL: SocketMonitorEventMask = SocketMonitorEventMask::ALL;
/// Convenience alias for [`SocketMonitorEventMask::CONNECTION_READY`].
pub const MONITOR_EVENT_CONNECTION_READY: SocketMonitorEventMask =
    SocketMonitorEventMask::CONNECTION_READY;

/// Options used when opening a socket monitor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SocketMonitorOpenOptions {
    /// Event types delivered by the monitor.
    pub events: SocketMonitorEventMask,
    /// Exact monitor queue HWM in bytes; zero selects Core's default.
    pub monitor_hwm_bytes: u64,
}

impl Default for SocketMonitorOpenOptions {
    fn default() -> Self {
        Self {
            events: SocketMonitorEventMask::ALL,
            monitor_hwm_bytes: 0,
        }
    }
}

/// A single socket connection-lifecycle event reported by a monitor.
#[derive(Debug, Clone)]
pub struct MonitorEvent {
    /// The kind of lifecycle event.
    pub event: MonitorEventType,
    /// An event-specific value, such as an error code or reconnect interval.
    pub value: u64,
    /// The peer routing id, when the event carries one.
    pub routing_id: Option<RoutingId>,
    /// The local endpoint address.
    pub local_addr: String,
    /// The remote endpoint address.
    pub remote_addr: String,
    /// A stable identifier for the underlying transport connection.
    pub connection_id: u64,
    /// The transport lane index, when the event applies to one.
    pub transport_lane: u32,
    /// Event-specific detail flags; see [`MonitorEventFlags`].
    pub flags: MonitorEventFlags,
}

impl MonitorEvent {
    /// Returns `true` when this event is a connection-established event.
    pub fn is_connected(&self) -> bool {
        self.event.0 & 0x0001 != 0
    }

    /// Returns `true` when this event is a peer-disconnected event.
    pub fn is_disconnected(&self) -> bool {
        self.event.0 & 0x0200 != 0
    }

    /// Returns `true` when this event is a started-listening event.
    pub fn is_listening(&self) -> bool {
        self.event.0 & 0x0008 != 0
    }

    /// Returns `true` when this event is an inbound-connection-accepted event.
    pub fn is_accepted(&self) -> bool {
        self.event.0 & 0x0020 != 0
    }

    /// Returns `true` when this event is a connection-closed event.
    pub fn is_closed(&self) -> bool {
        self.event.0 & 0x0080 != 0
    }

    /// Returns `true` when this event is a handshake-complete, ready-for-traffic
    /// event.
    pub fn is_connection_ready(&self) -> bool {
        self.event.0 & 0x1000 != 0
    }
}

/// The raw bitmask identifying a monitor event's kind.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MonitorEventType(pub u64);

/// Identifies what a monitored source is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MonitorSourceKind {
    /// A plain socket.
    Socket,
}

/// A point-in-time snapshot of a monitored entity's state and auto-high-water-
/// mark telemetry.
#[derive(Debug, Clone)]
pub struct MonitorStatus {
    /// ABI version of the native snapshot record.
    pub abi_version: u32,
    /// Native snapshot record size in bytes.
    pub struct_size: u32,
    /// What kind of source this snapshot describes.
    pub source_kind: MonitorSourceKind,
    /// Bit flags describing the source's current state.
    pub state_flags: u32,
    /// Bit flags carrying additional state detail.
    pub detail_flags: u32,
    /// The number of outbound messages currently queued.
    pub snd_pending_msgs: u64,
    /// The number of inbound messages currently queued.
    pub rcv_pending_msgs: u64,
    /// The number of outbound bytes currently queued.
    pub snd_pending_bytes: u64,
    /// The number of inbound bytes currently queued.
    pub rcv_pending_bytes: u64,
    /// Whether automatic high-water-mark sizing is enabled.
    pub auto_hwm_enabled: bool,
    /// The active auto-HWM profile.
    pub auto_hwm_profile: u32,
    /// The role the auto-HWM sizing inferred for this socket.
    pub auto_hwm_role: u32,
    /// The auto-HWM policy class in effect.
    pub auto_hwm_policy_class: u32,
    /// The send high-water mark planned by the current policy, in bytes.
    pub auto_hwm_planned_sndhwm_bytes: u64,
    /// The receive high-water mark planned by the current policy, in bytes.
    pub auto_hwm_planned_rcvhwm_bytes: u64,
    /// The send high-water mark currently applied, in bytes.
    pub auto_hwm_applied_sndhwm_bytes: u64,
    /// The receive high-water mark currently applied, in bytes.
    pub auto_hwm_applied_rcvhwm_bytes: u64,
    /// The effective OS send buffer size, in bytes.
    pub auto_hwm_effective_sndbuf: i32,
    /// The effective OS receive buffer size, in bytes.
    pub auto_hwm_effective_rcvbuf: i32,
    /// When the last auto-HWM recalculation ran, in milliseconds.
    pub auto_hwm_last_recalc_ms: u64,
    /// What triggered the last recalculation.
    pub auto_hwm_last_recalc_reason: AutoHwmRecalcReason,
    /// The fraction of sends blocked by back-pressure, in parts per million.
    pub auto_hwm_send_blocked_ratio_ppm: u32,
    /// A deferred send high-water mark target, in bytes.
    pub auto_hwm_deferred_sndhwm_bytes: u64,
    /// A deferred receive high-water mark target, in bytes.
    pub auto_hwm_deferred_rcvhwm_bytes: u64,
    /// Whether the deferred send target is valid.
    pub auto_hwm_deferred_sndhwm_valid: bool,
    /// Whether the deferred receive target is valid.
    pub auto_hwm_deferred_rcvhwm_valid: bool,
    /// Bytes retained by outbound pipe directions.
    pub snd_bytes_in_flight: u64,
    /// Bytes retained by inbound pipe directions.
    pub rcv_bytes_in_flight: u64,
    /// Minimum accounted charge for one Core frame.
    pub minimum_core_message_charge_bytes: u64,
    /// Number of messages admitted by the empty-pipe oversize rule.
    pub oversize_message_admission_count: u64,
    /// Largest accounted message admitted by the empty-pipe oversize rule.
    pub oversize_message_admission_max_bytes: u64,
    /// Current count of application pipes this socket sees as remote-PAUSED
    /// (paired DEALER/ROUTER completion lane; present since ABI 4).
    pub flow_paused_connections: u64,
    /// Total PAUSED transitions actually applied (never a stale/duplicate).
    pub flow_pause_applied_total: u64,
    /// Total RUNNING transitions actually applied (never a stale/duplicate).
    pub flow_resume_applied_total: u64,
    /// Total stale or duplicate flow-state frames ignored.
    pub flow_state_stale_total: u64,
    /// Duration of the most recently completed PAUSED interval, in
    /// milliseconds. 0 if no PAUSED interval has completed yet.
    pub flow_pause_duration_ms: u64,
}

/// Monitor handle for observing socket lifecycle and connection events.
///
/// The monitor is an independent observation plane that does not interfere
/// with the data plane.
pub struct SocketMonitor {
    pub(crate) inner: Box<MonitorStorage>,
}

impl SocketMonitor {
    /// Open a socket monitor for all events.
    pub fn open(socket: &dyn Monitorable) -> Result<Self, ConfigError> {
        crate::monitor::socket_monitor_open(socket)
    }

    /// Open a socket monitor with explicit event and byte-HWM options.
    pub fn open_with_options(
        socket: &dyn Monitorable,
        options: SocketMonitorOpenOptions,
    ) -> Result<Self, ConfigError> {
        crate::monitor::socket_monitor_open_with_options(socket, options)
    }

    /// Blocking receive of a monitor event.
    pub fn recv(&self) -> Result<MonitorEvent, RecvError> {
        self.inner.recv()
    }

    /// Non-blocking receive of a monitor event. Returns `Ok(None)` when no event is available.
    pub fn recv_with_flags(&self, flags: RecvFlags) -> Result<Option<MonitorEvent>, RecvError> {
        self.inner.recv_with_flags(flags)
    }

    /// Read the current monitor status.
    pub fn status(&self) -> Result<MonitorStatus, ConfigError> {
        self.inner.status()
    }

    /// Closes the monitor and releases its resources.
    pub fn close(&mut self) -> Result<(), CloseError> {
        self.inner.close()
    }
}

impl MonitorStatus {
    /// Returns `true` when the monitored socket is in the ready state.
    pub fn is_ready(&self) -> bool {
        crate::monitor::monitor_status_is_ready(self)
    }

    /// Returns `true` when the monitored socket is closed.
    pub fn is_closed(&self) -> bool {
        crate::monitor::monitor_status_is_closed(self)
    }

    /// Returns `true` when `flow_paused_connections` and the other `flow_*`
    /// fields are populated (ABI 4+, paired DEALER/ROUTER sockets only).
    pub fn has_flow_state_detail(&self) -> bool {
        crate::monitor::monitor_status_has_flow_state_detail(self)
    }
}

/// Typed bitmask for the event-specific flags carried in the native
/// `zlink_monitor_event_t.flags` field and projected onto
/// [`MonitorEvent::flags`] (core-byte-hwm-flow-control-plan.ko.md §5.1/§6).
/// Mirrors `zlink_monitor_event_flag_e` in
/// `core/include/zlink/eventing/api.h`. Combine with `|`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MonitorEventFlags(u32);

impl MonitorEventFlags {
    /// No event-specific flags are set.
    pub const NONE: Self = Self(0);
    /// Wraps a raw flag bitmask read from `zlink_monitor_event_t.flags`.
    pub(crate) const fn from_raw(bits: u32) -> Self {
        Self(bits)
    }
    /// Set on a `ConnectionReady` event that moves a connection from
    /// not-ready to ready.
    pub const CONNECTION_READY_EDGE: Self = Self(1 << 0);
    /// Set on `SendFlowResumed` when clearing the remote pause left the pipe
    /// actually writable. Clear when another cause (byte high-water-mark,
    /// transport wait, or termination) still blocks it.
    pub const SEND_FLOW_WRITABLE: Self = Self(1 << 1);
    /// Set on `FlowStateStale` when the frame named a different connection
    /// generation.
    pub const FLOW_STATE_STALE_GENERATION: Self = Self(1 << 2);
    /// Set on `FlowStateStale` when the epoch did not advance inside the
    /// current generation.
    pub const FLOW_STATE_STALE_EPOCH: Self = Self(1 << 3);

    /// Returns the raw flag bits.
    pub const fn bits(self) -> u32 {
        self.0
    }

    /// Returns `true` when `self` contains every bit set in `other`.
    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }
}

impl BitOr for MonitorEventFlags {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl BitOrAssign for MonitorEventFlags {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}
