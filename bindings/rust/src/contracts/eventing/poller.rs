use std::any::Any;
use std::os::fd::RawFd;

use crate::internal::PollerStorage;

pub(crate) mod private {
    pub(crate) trait Sealed {}
}

/// Poll event flag: the source is readable (a receive will not block).
pub const POLLIN: i16 = 1;
/// Poll event flag: socket-wide writable readiness is present.
///
/// For a DONTWAIT SEND or REQUEST this aggregate level hint can represent an
/// unread WRITABLE record. The binding pulls the completion queue and resumes
/// only operations whose exact token, context, and routed target match.
pub const POLLOUT: i16 = 2;
/// Poll event flag: a completion queue record is available. REQUEST uses this
/// for both WRITABLE admission retries and its terminal result; SEND
/// backpressure is resumed by WRITABLE on [`POLLOUT`].
pub const POLLCOMPLETION: i16 = 32;
/// Poll event flag: a ROUTER's selected route changed; read
/// `RouterSocket::routes_snapshot`.
pub const POLLROUTE: i16 = 64;
/// Poll event flag: outbound connection attempts changed; read
/// `connection_attempts_snapshot`.
pub const POLLCONNECT: i16 = 128;

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
    /// The event came from a socket or socket monitor.
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

/// Poller for monitoring socket and monitor readiness, file descriptors, and timers.
pub struct Poller {
    pub(crate) inner: Box<PollerStorage>,
}
