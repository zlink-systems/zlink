use std::any::Any;
use std::os::fd::RawFd;
use std::sync::Arc;

use crate::error::{ConfigError, HandlerError, RecvError};
use crate::internal::{PollerStorage, TimerStorage};

pub(crate) mod private {
    pub(crate) trait Sealed {}
}

/// Poll event flag: the source is readable (a receive will not block).
pub const POLLIN: i16 = 1;
/// Poll event flag: the source is writable (a send will not block).
pub const POLLOUT: i16 = 2;
/// Poll event flag: an asynchronous operation completed.
pub const POLLCOMPLETION: i16 = 32;

/// A built-in socket source that can be registered with a [`Poller`].
///
/// The trait is sealed because the binding must map a source to its native
/// socket handle; custom implementations cannot provide that mapping safely.
#[allow(private_bounds)]
pub trait Pollable: Any + private::Sealed {
    /// Internal type identity used to map the built-in socket source to Core.
    #[doc(hidden)]
    fn as_any(&self) -> &dyn Any;
}

/// Source kind reported by the poller when an event fires.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PollSourceKind {
    /// The event came from a socket.
    Socket,
    /// The event came from a raw file descriptor.
    Fd,
    /// The event came from a timer.
    Timer,
}

/// A single ready source reported by [`Poller::wait`].
#[derive(Debug, Clone, Copy)]
pub struct PollEvent {
    /// What kind of source fired.
    pub source_kind: PollSourceKind,
    /// The file descriptor, when the source is a raw fd.
    pub fd: RawFd,
    /// The caller token supplied when the source was registered.
    pub slot: usize,
    /// The events that fired, as a mask of `POLL*` flags.
    pub revents: i16,
}

impl PollEvent {
    /// Returns `true` when the source is readable ([`POLLIN`] is set).
    pub fn is_readable(&self) -> bool {
        self.revents & POLLIN != 0
    }

    /// Returns `true` when the source is writable ([`POLLOUT`] is set).
    pub fn is_writable(&self) -> bool {
        self.revents & POLLOUT != 0
    }
}

impl Default for PollEvent {
    fn default() -> Self {
        Self {
            source_kind: PollSourceKind::Socket,
            fd: 0,
            slot: 0,
            revents: 0,
        }
    }
}

/// A raw poll descriptor: a file descriptor, the events to watch, and the
/// events that fired.
#[derive(Clone, Copy)]
pub struct PollItem {
    /// The file descriptor to poll.
    pub fd: RawFd,
    /// The events to watch for, as a mask of `POLL*` flags.
    pub events: i16,
    /// The events that fired, as a mask of `POLL*` flags.
    pub revents: i16,
}

/// Poller for monitoring socket readiness, file descriptors, and timers.
pub struct Poller {
    pub(crate) inner: Box<PollerStorage>,
}

impl Poller {
    /// Creates an empty poller. The caller owns it and releases it on drop.
    pub fn new() -> Result<Self, ConfigError> {
        crate::poller::poller_new()
    }

    /// Registers `socket` to be watched for `events`; `slot` is a caller token
    /// echoed back in the matching [`PollEvent`].
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
    /// A registration that adds or removes [`POLLCOMPLETION`] must instead be
    /// removed and added again because completion processing has separate
    /// ownership in Core.
    pub fn modify_socket(&self, socket: &dyn Pollable, events: i16) -> Result<(), ConfigError> {
        self.inner.modify_socket(socket, events)
    }

    /// Remove a socket from the poller.
    pub fn remove_socket(&self, socket: &dyn Pollable) -> Result<(), ConfigError> {
        self.inner.remove_socket(socket)
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
    pub fn wait(&self, events: &mut [PollEvent], timeout_ms: i64) -> Result<usize, RecvError> {
        self.inner.wait(events, timeout_ms)
    }

    /// Returns the number of registered sources.
    pub fn size(&self) -> i32 {
        self.inner.size()
    }
}

/// A timer that fires on an interval and can be polled or awaited.
pub struct Timer {
    pub(crate) inner: Arc<TimerStorage>,
}

impl Timer {
    /// Creates a standalone timer. The caller owns it and releases it on drop.
    pub fn new() -> Result<Self, ConfigError> {
        crate::poller::timer_new()
    }

    /// Starts the timer firing once per `interval_ns` nanoseconds;
    /// `repeat_count` sets how many times it fires.
    pub fn start(&self, interval_ns: u64, repeat_count: u64) -> Result<(), ConfigError> {
        self.inner.start(interval_ns, repeat_count)
    }

    /// Stops the timer; it can be restarted with [`start`](Self::start).
    pub fn stop(&self) -> Result<(), ConfigError> {
        self.inner.stop()
    }

    /// Receive a timer fire count. Returns `Ok(None)` when no data is available.
    pub fn recv(&self) -> Result<Option<u64>, RecvError> {
        self.inner.recv()
    }

    /// Registers a callback invoked on each expiration with the timer and the
    /// cumulative fire count. The callback runs on a background dispatch thread.
    pub fn on_fire<F>(&mut self, handler: F) -> Result<(), HandlerError>
    where
        F: Fn(&Timer, u64) + Send + 'static,
    {
        let timer = Arc::downgrade(&self.inner);
        self.inner.on_fire(timer, handler)
    }
}
