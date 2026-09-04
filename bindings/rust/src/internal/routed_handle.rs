// SPDX-License-Identifier: MPL-2.0

//! Shared ownership of one DEALER/ROUTER native handle.
//!
//! This is a handle holder only. It owns no thread, queue, retry policy, or
//! deadline timer. SEND retry state belongs to the operation/completion owner;
//! REQUEST deadlines remain owned by Core.

use std::ffi::c_void;
use std::sync::Arc;
use std::sync::atomic::{AtomicPtr, Ordering};

/// Keeps the native handle reachable from operation builders that outlive the
/// socket value. Core owns submit admission and lifecycle concurrency.
pub(crate) struct RoutedHandle {
    handle: AtomicPtr<c_void>,
}

unsafe impl Send for RoutedHandle {}
unsafe impl Sync for RoutedHandle {}

impl RoutedHandle {
    pub(crate) fn new(handle: *mut c_void) -> Arc<Self> {
        Arc::new(Self {
            handle: AtomicPtr::new(handle),
        })
    }

    pub(crate) fn handle(&self) -> *mut c_void {
        self.handle.load(Ordering::Acquire)
    }

    pub(crate) fn close_native(&self) -> i32 {
        let handle = self.handle();
        if handle.is_null() {
            return 0;
        }
        let rc = unsafe { crate::ffi::zlink_close(handle) };
        if rc == 0 {
            self.handle.store(std::ptr::null_mut(), Ordering::Release);
        }
        rc
    }

    pub(crate) fn detach(&self) {
        self.handle.store(std::ptr::null_mut(), Ordering::Release);
    }
}
