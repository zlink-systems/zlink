// SPDX-License-Identifier: MPL-2.0

//! Shared ownership of one DEALER/ROUTER native handle.
//!
//! This is a handle holder only. It owns no thread, no queue, no retry policy
//! and no deadline timer. The 0.13.1 contract makes the binding a pure Core
//! wrapper and hands every send completion, every HWM wait and every deadline
//! to Core.

use std::ffi::c_void;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::ffi;
use crate::message::RoutingId;
use crate::native_errors::submit_error_from_errno;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RoutedRole {
    Dealer,
    Router,
}

/// Keeps the native handle reachable from operation builders that outlive the
/// socket value, and serialises the multi-call request submit sequence so two
/// concurrent requests cannot interleave their parts on the same handle.
pub(crate) struct RoutedHandle {
    handle: AtomicPtr<c_void>,
    role: RoutedRole,
    submit_gate: Mutex<()>,
}

unsafe impl Send for RoutedHandle {}
unsafe impl Sync for RoutedHandle {}

impl RoutedHandle {
    pub(crate) fn new(handle: *mut c_void, role: RoutedRole) -> Arc<Self> {
        Arc::new(Self {
            handle: AtomicPtr::new(handle),
            role,
            submit_gate: Mutex::new(()),
        })
    }

    pub(crate) fn role(&self) -> RoutedRole {
        self.role
    }

    pub(crate) fn handle(&self) -> *mut c_void {
        self.handle.load(Ordering::Acquire)
    }

    /// Runs `attempt` with the live handle under the outbound gate. Returns
    /// `None` once the handle has been closed and detached.
    pub(crate) fn with_submit_gate<T>(&self, attempt: impl FnOnce(*mut c_void) -> T) -> Option<T> {
        let _gate = self.submit_gate.lock().expect("routed submit gate");
        let handle = self.handle();
        (!handle.is_null()).then(|| attempt(handle))
    }

    /// Snapshots one exact routed target. The value claims no pipe credit; a
    /// later submit still observes the Core HWM contract.
    pub(crate) fn select_target(
        &self,
        router_rid: Option<&RoutingId>,
    ) -> (i32, ffi::zlink_routed_submit_target_t) {
        let _gate = self.submit_gate.lock().expect("routed submit gate");
        let handle = self.handle();
        let mut target = ffi::zlink_routed_submit_target_t {
            peer_rid: ffi::zlink_routing_id_t::empty(),
            transport_pair_id: 0,
            transport_pair_generation: 0,
        };
        if handle.is_null() {
            return (-1, target);
        }
        let rid = router_rid
            .map(|value| value.as_raw() as *const ffi::zlink_routing_id_t)
            .unwrap_or(std::ptr::null());
        let rc = unsafe { ffi::zlink_select_routed_submit_target(handle, rid, &mut target) };
        (rc, target)
    }

    /// Core-owned request deadline (`ZLINK_REQUEST_TIMED_OUT` fires it).
    pub(crate) fn request_timeout(&self) -> Result<Duration, crate::error::SubmitError> {
        let _gate = self.submit_gate.lock().expect("routed submit gate");
        let handle = self.handle();
        if handle.is_null() {
            return Err(submit_error_from_errno(libc::ECANCELED));
        }
        let mut millis = 0i32;
        let mut len = std::mem::size_of::<i32>();
        let rc = unsafe {
            match self.role {
                RoutedRole::Dealer => ffi::zlink_get_dealer_option(
                    handle,
                    ffi::zlink_dealer_option_t::ZLINK_DEALER_OPT_REQUEST_TIMEOUT_MS,
                    (&mut millis as *mut i32).cast(),
                    &mut len,
                ),
                RoutedRole::Router => ffi::zlink_get_router_option(
                    handle,
                    ffi::zlink_router_option_t::ZLINK_ROUTER_OPT_REQUEST_TIMEOUT_MS,
                    (&mut millis as *mut i32).cast(),
                    &mut len,
                ),
            }
        };
        if rc != 0 {
            return Err(submit_error_from_errno(unsafe { ffi::zlink_errno() }));
        }
        Ok(Duration::from_millis(millis.max(0) as u64))
    }

    pub(crate) fn close_native(&self, detach_on_failure: bool) -> i32 {
        let _gate = self.submit_gate.lock().expect("routed submit gate");
        let handle = self.handle();
        if handle.is_null() {
            return 0;
        }
        let rc = unsafe { ffi::zlink_close(handle) };
        if rc == 0 || detach_on_failure {
            self.handle.store(std::ptr::null_mut(), Ordering::Release);
        }
        rc
    }

    pub(crate) fn detach(&self) {
        self.handle.store(std::ptr::null_mut(), Ordering::Release);
    }
}
