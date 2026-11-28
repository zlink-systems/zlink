use std::sync::Arc;

use crate::internal::TimerStorage;
use crate::{ConfigError, RecvError};

/// A timer that fires on an interval and can be polled or awaited.
pub struct Timer {
    pub(crate) inner: Arc<TimerStorage>,
}

impl Timer {
    /// Creates a standalone timer. The caller owns it and releases it on drop.
    pub fn new() -> Result<Self, ConfigError> {
        TimerStorage::new_timer()
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
}
