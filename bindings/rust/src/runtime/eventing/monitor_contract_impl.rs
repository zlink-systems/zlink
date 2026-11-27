// SPDX-License-Identifier: MPL-2.0

use crate::internal::MonitorStorage;
use crate::{
    CloseError, ConfigError, MonitorEvent, MonitorStatus, Monitorable, RecvError, RecvFlags,
    SocketMonitor, SocketMonitorOpenOptions,
};

impl SocketMonitor {
    /// Open a socket monitor for all events.
    pub fn open(socket: &dyn Monitorable) -> Result<Self, ConfigError> {
        MonitorStorage::open(socket)
    }

    /// Open a socket monitor with explicit event and byte-HWM options.
    pub fn open_with_options(
        socket: &dyn Monitorable,
        options: SocketMonitorOpenOptions,
    ) -> Result<Self, ConfigError> {
        MonitorStorage::open_with_options(socket, options)
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
        self.native_is_ready()
    }

    /// Returns `true` when the monitored socket is closed.
    pub fn is_closed(&self) -> bool {
        self.native_is_closed()
    }

    /// Returns `true` when `flow_paused_connections` and the other `flow_*`
    /// fields are populated (ABI 4+, paired DEALER/ROUTER sockets only).
    pub fn has_flow_state_detail(&self) -> bool {
        self.native_has_flow_state_detail()
    }
}
