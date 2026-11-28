use crate::contracts::eventing::poller::Pollable;
use crate::internal::{AtomicCounterStorage, CoreUtilities, StopwatchStorage, ThreadStorage};
use crate::runtime::errors::native_errors::check_config_rc;
use crate::runtime::eventing::poller::pollable_handle;

use crate::contracts::errors::errors::{ConfigError, ConfigResult};
use crate::runtime::native::ffi;
use std::ffi::{CStr, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
use std::ptr;

impl StopwatchStorage {
    pub(crate) fn start() -> Result<Self, ConfigError> {
        let handle = unsafe { ffi::zlink_stopwatch_start() };
        if handle.is_null() {
            return Err(ConfigError::new(ConfigResult::InvalidHandle, unsafe {
                ffi::zlink_errno()
            }));
        }
        Ok(Self { handle })
    }

    pub(crate) fn intermediate(&self) -> u64 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { ffi::zlink_stopwatch_intermediate(self.handle) as u64 }
    }

    pub(crate) fn stop(&mut self) -> u64 {
        if self.handle.is_null() {
            return 0;
        }
        let handle = self.handle;
        self.handle = ptr::null_mut();
        unsafe { ffi::zlink_stopwatch_stop(handle) as u64 }
    }

    pub(crate) fn close(&mut self) {
        let _ = self.stop();
    }
}

impl Drop for StopwatchStorage {
    fn drop(&mut self) {
        self.close();
    }
}

impl AtomicCounterStorage {
    pub(crate) fn new() -> Result<Self, ConfigError> {
        let handle = unsafe { ffi::zlink_atomic_counter_new() };
        if handle.is_null() {
            return Err(ConfigError::new(ConfigResult::InvalidHandle, unsafe {
                ffi::zlink_errno()
            }));
        }
        Ok(Self { handle })
    }

    pub(crate) fn set(&self, value: i32) {
        if self.handle.is_null() {
            return;
        }
        unsafe { ffi::zlink_atomic_counter_set(self.handle, value) };
    }

    pub(crate) fn increment(&self) -> i32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { ffi::zlink_atomic_counter_inc(self.handle) }
    }

    pub(crate) fn decrement(&self) -> i32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { ffi::zlink_atomic_counter_dec(self.handle) }
    }

    pub(crate) fn value(&self) -> i32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { ffi::zlink_atomic_counter_value(self.handle) }
    }

    pub(crate) fn close(&mut self) {
        if self.handle.is_null() {
            return;
        }
        let mut handle = self.handle;
        self.handle = ptr::null_mut();
        unsafe { ffi::zlink_atomic_counter_destroy(&mut handle) };
    }
}

impl Drop for AtomicCounterStorage {
    fn drop(&mut self) {
        self.close();
    }
}

struct ThreadState {
    task: Option<Box<dyn FnOnce() + Send + 'static>>,
    panic: Option<Box<dyn std::any::Any + Send + 'static>>,
}

extern "C" fn thread_entry(userdata: *mut c_void) {
    let state = unsafe { &mut *(userdata as *mut ThreadState) };
    let task = state.task.take();
    if let Some(task) = task {
        if let Err(panic) = catch_unwind(AssertUnwindSafe(task)) {
            state.panic = Some(panic);
        }
    }
}

impl ThreadStorage {
    pub(crate) fn start<F>(task: F) -> Result<Self, ConfigError>
    where
        F: FnOnce() + Send + 'static,
    {
        let state = Box::into_raw(Box::new(ThreadState {
            task: Some(Box::new(task)),
            panic: None,
        }));
        let handle = unsafe { ffi::zlink_thread_start(Some(thread_entry), state.cast()) };
        if handle.is_null() {
            unsafe {
                drop(Box::from_raw(state));
            }
            return Err(ConfigError::new(ConfigResult::InvalidHandle, unsafe {
                ffi::zlink_errno()
            }));
        }
        Ok(Self {
            handle,
            state: state.cast(),
        })
    }

    pub(crate) fn join(&mut self) {
        if self.handle.is_null() {
            return;
        }
        let handle = self.handle;
        self.handle = ptr::null_mut();
        unsafe { ffi::zlink_thread_join(handle) };
        let state = unsafe { Box::from_raw(self.state.cast::<ThreadState>()) };
        self.state = ptr::null_mut();
        if let Some(panic) = state.panic {
            resume_unwind(panic);
        }
    }

    pub(crate) fn close(&mut self) {
        if self.handle.is_null() {
            return;
        }
        let _ = catch_unwind(AssertUnwindSafe(|| self.join()));
    }
}

impl Drop for ThreadStorage {
    fn drop(&mut self) {
        self.close();
        if !self.state.is_null() {
            unsafe {
                drop(Box::from_raw(self.state.cast::<ThreadState>()));
            }
            self.state = ptr::null_mut();
        }
    }
}

impl CoreUtilities {
    pub(crate) fn proxy(
        frontend: &dyn Pollable,
        backend: &dyn Pollable,
        capture: Option<&dyn Pollable>,
    ) -> Result<(), ConfigError> {
        let frontend = pollable_handle(frontend)?;
        let backend = pollable_handle(backend)?;
        let capture = capture
            .map(pollable_handle)
            .transpose()?
            .unwrap_or(std::ptr::null_mut());
        check_config_rc(unsafe { ffi::zlink_proxy(frontend, backend, capture) })
    }

    pub(crate) fn sleep(seconds: i32) {
        unsafe {
            ffi::zlink_sleep(seconds);
        }
    }

    /// Return the runtime library version as `(major, minor, patch)`.
    pub(crate) fn version() -> (i32, i32, i32) {
        let (mut major, mut minor, mut patch) = (0i32, 0i32, 0i32);
        unsafe {
            ffi::zlink_version(&mut major, &mut minor, &mut patch);
        }
        (major, minor, patch)
    }

    /// Check whether the library supports a given capability (e.g. `"ipc"`, `"tls"`).
    pub(crate) fn has(capability: &str) -> bool {
        let c = std::ffi::CString::new(capability).unwrap_or_default();
        unsafe { ffi::zlink_has(c.as_ptr()) }
    }

    /// Returns the native error text for an errno value.
    pub(crate) fn strerror(errnum: i32) -> &'static str {
        let ptr = unsafe { ffi::zlink_strerror(errnum) };
        if ptr.is_null() {
            ""
        } else {
            unsafe { CStr::from_ptr(ptr) }.to_str().unwrap_or("")
        }
    }
}
