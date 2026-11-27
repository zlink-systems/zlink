use crate::Timer;
use crate::contracts::errors::errors::{ConfigError, RecvError, RecvResult};
use crate::internal::TimerStorage;
use crate::runtime::errors::native_errors::{check_config_rc, check_recv_rc, last_errno};
use crate::runtime::native::ffi;
use std::sync::Arc;

impl TimerStorage {
    pub(crate) fn new_timer() -> Result<Timer, ConfigError> {
        let handle = unsafe { ffi::zlink_timer_new() };
        if handle.is_null() {
            return Err(crate::contracts::errors::errors::ConfigError::new(
                crate::contracts::errors::errors::ConfigResult::InvalidArgument,
                last_errno(),
            ));
        }
        Ok(Timer {
            inner: Arc::new(TimerStorage { handle }),
        })
    }

    pub(crate) fn start(&self, interval_ns: u64, repeat_count: u64) -> Result<(), ConfigError> {
        check_config_rc(unsafe { ffi::zlink_timer_start(self.handle, interval_ns, repeat_count) })
    }

    pub(crate) fn stop(&self) -> Result<(), ConfigError> {
        check_config_rc(unsafe { ffi::zlink_timer_stop(self.handle) })
    }

    /// Receive a timer fire count. Returns `Ok(None)` when Core reports `RECV_NO_DATA`.
    pub(crate) fn recv(&self) -> Result<Option<u64>, RecvError> {
        let mut count = 0u64;
        let rc = unsafe { ffi::zlink_timer_recv(self.handle, &mut count) };
        if rc == RecvResult::NoData as i32 {
            return Ok(None);
        }
        check_recv_rc(rc)?;
        Ok(Some(count))
    }
}

impl Drop for TimerStorage {
    fn drop(&mut self) {
        // Core destroy is fail-fast; a drop cannot report or retry its result.
        let mut handle = self.handle;
        unsafe { ffi::zlink_timer_destroy(&mut handle) };
    }
}
