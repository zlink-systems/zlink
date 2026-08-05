use crate::error::{ConfigError, ConfigResult};
use crate::ffi;
use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind, resume_unwind};
use std::ptr;

/// A high-resolution elapsed-time stopwatch.
pub struct Stopwatch {
    handle: *mut c_void,
}

impl Stopwatch {
    pub fn start() -> Result<Self, ConfigError> {
        let handle = unsafe { ffi::zlink_stopwatch_start() };
        if handle.is_null() {
            return Err(ConfigError::new(ConfigResult::InvalidHandle, unsafe {
                ffi::zlink_errno()
            }));
        }
        Ok(Self { handle })
    }

    pub fn intermediate(&self) -> u64 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { ffi::zlink_stopwatch_intermediate(self.handle) as u64 }
    }

    pub fn stop(&mut self) -> u64 {
        if self.handle.is_null() {
            return 0;
        }
        let handle = self.handle;
        self.handle = ptr::null_mut();
        unsafe { ffi::zlink_stopwatch_stop(handle) as u64 }
    }

    pub fn close(&mut self) {
        let _ = self.stop();
    }
}

impl Drop for Stopwatch {
    fn drop(&mut self) {
        self.close();
    }
}

/// A thread-safe integer counter.
pub struct AtomicCounter {
    handle: *mut c_void,
}

impl AtomicCounter {
    pub fn new() -> Result<Self, ConfigError> {
        let handle = unsafe { ffi::zlink_atomic_counter_new() };
        if handle.is_null() {
            return Err(ConfigError::new(ConfigResult::InvalidHandle, unsafe {
                ffi::zlink_errno()
            }));
        }
        Ok(Self { handle })
    }

    pub fn set(&self, value: i32) {
        if self.handle.is_null() {
            return;
        }
        unsafe { ffi::zlink_atomic_counter_set(self.handle, value) };
    }

    pub fn increment(&self) -> i32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { ffi::zlink_atomic_counter_inc(self.handle) }
    }

    pub fn decrement(&self) -> i32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { ffi::zlink_atomic_counter_dec(self.handle) }
    }

    pub fn value(&self) -> i32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { ffi::zlink_atomic_counter_value(self.handle) }
    }

    pub fn close(&mut self) {
        if self.handle.is_null() {
            return;
        }
        let mut handle = self.handle;
        self.handle = ptr::null_mut();
        unsafe { ffi::zlink_atomic_counter_destroy(&mut handle) };
    }
}

impl Drop for AtomicCounter {
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

/// A running background thread created by the zlink runtime.
pub struct Thread {
    handle: *mut c_void,
    state: *mut ThreadState,
}

impl Thread {
    pub fn start<F>(task: F) -> Result<Self, ConfigError>
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
        Ok(Self { handle, state })
    }

    pub fn join(&mut self) {
        if self.handle.is_null() {
            return;
        }
        let handle = self.handle;
        self.handle = ptr::null_mut();
        unsafe { ffi::zlink_thread_join(handle) };
        let state = unsafe { Box::from_raw(self.state) };
        self.state = ptr::null_mut();
        if let Some(panic) = state.panic {
            resume_unwind(panic);
        }
    }

    pub fn close(&mut self) {
        if self.handle.is_null() {
            return;
        }
        let _ = catch_unwind(AssertUnwindSafe(|| self.join()));
    }
}

impl Drop for Thread {
    fn drop(&mut self) {
        self.close();
        if !self.state.is_null() {
            unsafe {
                drop(Box::from_raw(self.state));
            }
            self.state = ptr::null_mut();
        }
    }
}
