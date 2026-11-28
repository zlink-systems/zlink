use crate::ConfigError;
use crate::internal::{AtomicCounterStorage, CoreUtilities, StopwatchStorage, ThreadStorage};

/// A high-resolution elapsed-time stopwatch.
pub struct Stopwatch {
    inner: StopwatchStorage,
}

impl Stopwatch {
    pub fn start() -> Result<Self, ConfigError> {
        Ok(Self {
            inner: StopwatchStorage::start()?,
        })
    }

    pub fn intermediate(&self) -> u64 {
        self.inner.intermediate()
    }

    pub fn stop(&mut self) -> u64 {
        self.inner.stop()
    }

    pub fn close(&mut self) {
        self.inner.close();
    }
}

/// A thread-safe integer counter.
pub struct AtomicCounter {
    inner: AtomicCounterStorage,
}

impl AtomicCounter {
    pub fn new() -> Result<Self, ConfigError> {
        Ok(Self {
            inner: AtomicCounterStorage::new()?,
        })
    }

    pub fn set(&self, value: i32) {
        self.inner.set(value);
    }

    pub fn increment(&self) -> i32 {
        self.inner.increment()
    }

    pub fn decrement(&self) -> i32 {
        self.inner.decrement()
    }

    pub fn value(&self) -> i32 {
        self.inner.value()
    }

    pub fn close(&mut self) {
        self.inner.close();
    }
}

/// A running background thread created by the zlink runtime.
pub struct Thread {
    inner: ThreadStorage,
}

impl Thread {
    pub fn start<F>(task: F) -> Result<Self, ConfigError>
    where
        F: FnOnce() + Send + 'static,
    {
        Ok(Self {
            inner: ThreadStorage::start(task)?,
        })
    }

    pub fn join(&mut self) {
        self.inner.join();
    }

    pub fn close(&mut self) {
        self.inner.close();
    }
}

/// Runs a blocking proxy between two sockets.
pub fn proxy(
    frontend: &dyn crate::Pollable,
    backend: &dyn crate::Pollable,
    capture: Option<&dyn crate::Pollable>,
) -> Result<(), crate::ConfigError> {
    CoreUtilities::proxy(frontend, backend, capture)
}

/// Suspends the calling thread for whole seconds.
pub fn sleep(seconds: i32) {
    CoreUtilities::sleep(seconds);
}

/// Returns the linked Core version as major, minor and patch numbers.
pub fn version() -> (i32, i32, i32) {
    CoreUtilities::version()
}

/// Reports whether Core supports the named capability.
pub fn has(capability: &str) -> bool {
    CoreUtilities::has(capability)
}

/// Returns Core's message for the given errno.
pub fn strerror(errnum: i32) -> &'static str {
    CoreUtilities::strerror(errnum)
}
