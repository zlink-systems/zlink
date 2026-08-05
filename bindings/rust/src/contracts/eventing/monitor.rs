use std::any::Any;
use std::ops::{BitOr, BitOrAssign};

use crate::core_context::AutoHwmRecalcReason;
use crate::error::{CloseError, ConfigError, HandlerError, RecvError};
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
    pub const ALL: Self = Self(0x7FFF);
    /// Subscribes only to the connection-ready event.
    pub const CONNECTION_READY: Self = Self(0x1000);

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

/// A single socket connection-lifecycle event reported by a monitor.
#[derive(Debug, Clone)]
pub struct MonitorEvent {
    /// The kind of lifecycle event.
    pub event: MonitorEventType,
    /// An event-specific value, such as an error code or reconnect interval.
    pub value: u32,
    /// The peer routing id, when the event carries one.
    pub routing_id: Option<RoutingId>,
    /// The local endpoint address.
    pub local_addr: String,
    /// The remote endpoint address.
    pub remote_addr: String,
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
    /// Whether automatic high-water-mark sizing is enabled.
    pub auto_hwm_enabled: bool,
    /// The active auto-HWM profile.
    pub auto_hwm_profile: u32,
    /// The role the auto-HWM sizing inferred for this socket.
    pub auto_hwm_role: u32,
    /// The auto-HWM policy class in effect.
    pub auto_hwm_policy_class: u32,
    /// The per-unit memory budget, in bytes, used when sizing.
    pub auto_hwm_unit_budget_bytes: u64,
    /// The upper cap applied to auto-sized high-water marks.
    pub auto_hwm_size_cap: u32,
    /// The number of message slots auto-sizing allotted to the socket.
    pub auto_hwm_socket_message_slots: u64,
    /// Whether a connection-count bucket applied to the socket plan.
    pub auto_hwm_connection_bucket_enabled: bool,
    /// The peer count used to choose the connection bucket.
    pub auto_hwm_connection_bucket_count: u32,
    /// The selected connection bucket index, or `u32::MAX` when no bucket applies.
    pub auto_hwm_connection_bucket_index: u32,
    /// The selected bucket HWM for a 4 KiB message unit.
    pub auto_hwm_connection_bucket_hwm_4k: u32,
    /// Whether hysteresis retained the previous connection bucket.
    pub auto_hwm_connection_bucket_hysteresis_retained: bool,
    /// The effective per-message size, in bytes, used when sizing.
    pub auto_hwm_effective_message_bytes: u64,
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
}

fn ignore_monitor_event(_: &MonitorEvent) {}

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

    /// Returns a snapshot of the monitored socket's current status; an alias for
    /// [`status`](Self::status).
    pub fn snapshot(&self) -> Result<MonitorStatus, ConfigError> {
        self.status()
    }

    /// Install a callback handler for monitor events.
    ///
    /// The callback runs on a background dispatch thread.
    pub fn on_event<F>(&mut self, handler: F) -> Result<(), HandlerError>
    where
        F: Fn(&MonitorEvent) + Send + 'static,
    {
        self.inner.on_event(handler)
    }

    /// Returns a no-op event handler that ignores every event.
    pub fn ignore_handler() -> fn(&MonitorEvent) {
        ignore_monitor_event
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
}
