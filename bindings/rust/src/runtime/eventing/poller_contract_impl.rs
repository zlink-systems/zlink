// SPDX-License-Identifier: MPL-2.0

use crate::internal::PollerStorage;
use crate::{
    CloseError, ConfigError, PollEvent, Pollable, Poller, SocketMonitor, Timer, ZlinkError,
};
use std::os::fd::RawFd;

impl Poller {
    /// Creates an empty poller. The caller owns it and releases it on drop.
    pub fn new() -> Result<Self, ConfigError> {
        PollerStorage::new_poller()
    }

    /// Destroys the poller. A native `EBUSY` leaves it available for a later close.
    pub fn close(&mut self) -> Result<(), CloseError> {
        self.inner.close()
    }

    /// Registers `socket` to be watched for `events`; `slot` is a caller token
    /// echoed back in the matching [`PollEvent`].
    ///
    /// Register both [`crate::POLLOUT`] and [`crate::POLLCOMPLETION`] when this poller owns
    /// progress for managed SEND admission or REQUEST admission/reply stages.
    pub fn add_socket(
        &self,
        socket: &dyn Pollable,
        events: i16,
        slot: usize,
    ) -> Result<(), ConfigError> {
        self.inner.add_socket(socket, events, slot)
    }

    /// Modifies the event mask for a previously added socket.
    ///
    /// Adding or removing [`crate::POLLCOMPLETION`] transfers the socket completion
    /// drain owner atomically with the native registration change. Register
    /// [`crate::POLLOUT`] with it when this poller drives backpressured SEND or REQUEST
    /// futures.
    pub fn modify_socket(&self, socket: &dyn Pollable, events: i16) -> Result<(), ConfigError> {
        self.inner.modify_socket(socket, events)
    }

    /// Remove a socket from the poller.
    pub fn remove_socket(&self, socket: &dyn Pollable) -> Result<(), ConfigError> {
        self.inner.remove_socket(socket)
    }

    /// Registers a socket monitor for readable events; `slot` is echoed back
    /// in the matching [`PollEvent`]. Only [`crate::POLLIN`] is valid for monitors.
    pub fn add_monitor(
        &self,
        monitor: &SocketMonitor,
        events: i16,
        slot: usize,
    ) -> Result<(), ConfigError> {
        self.inner.add_monitor(monitor, events, slot)
    }

    /// Modifies a previously added socket monitor. Only [`crate::POLLIN`] is valid.
    pub fn modify_monitor(&self, monitor: &SocketMonitor, events: i16) -> Result<(), ConfigError> {
        self.inner.modify_monitor(monitor, events)
    }

    /// Removes a socket monitor from the poller.
    pub fn remove_monitor(&self, monitor: &SocketMonitor) -> Result<(), ConfigError> {
        self.inner.remove_monitor(monitor)
    }

    /// Registers raw file descriptor `fd` to be watched for `events`; `slot` is
    /// echoed back in the matching [`PollEvent`].
    pub fn add_fd(&self, fd: RawFd, events: i16, slot: usize) -> Result<(), ConfigError> {
        self.inner.add_fd(fd, events, slot)
    }

    /// Changes the watched events for an already-registered file descriptor.
    pub fn modify_fd(&self, fd: RawFd, events: i16) -> Result<(), ConfigError> {
        self.inner.modify_fd(fd, events)
    }

    /// Unregisters file descriptor `fd`.
    pub fn remove_fd(&self, fd: RawFd) -> Result<(), ConfigError> {
        self.inner.remove_fd(fd)
    }

    /// Registers `timer`; its expirations surface as poll events tagged with
    /// `slot`.
    pub fn add_timer(&self, timer: &Timer, slot: usize) -> Result<(), ConfigError> {
        self.inner.add_timer(timer, slot)
    }

    /// Unregisters `timer`.
    pub fn remove_timer(&self, timer: &Timer) -> Result<(), ConfigError> {
        self.inner.remove_timer(timer)
    }

    /// Waits up to `timeout_ms` milliseconds for sources to become ready,
    /// writing up to `events.len()` results into `events`; a negative timeout
    /// blocks indefinitely. Returns the number of ready sources written.
    /// Before returning a socket's [`crate::POLLOUT`] or [`crate::POLLCOMPLETION`] event,
    /// the owning poller pulls completion records through NO_DATA so matching
    /// WRITABLE waiters can retry and admitted REQUEST waiters can receive
    /// their reply or terminal result. Native wait failures retain Core's
    /// configuration result; completion drain failures are receive errors.
    pub fn wait(&self, events: &mut [PollEvent], timeout_ms: i64) -> Result<usize, ZlinkError> {
        self.inner.wait(events, timeout_ms)
    }

    /// Returns the number of registered sources, or Core's configuration error.
    pub fn size(&self) -> Result<i32, ConfigError> {
        self.inner.size()
    }
}
