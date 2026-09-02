use std::cell::UnsafeCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::{Arc, Mutex};

use crate::ffi;

#[derive(Clone)]
pub(crate) struct PollerSocketRegistration {
    pub(crate) events: i16,
    pub(crate) completion_owner: Option<Arc<super::CompletionOwner>>,
}

pub(crate) struct ContextStorage {
    pub(crate) handle: *mut c_void,
    /// Cached thread name prefix (write-only in the C API; readable from cache).
    pub(crate) thread_name_prefix: Mutex<String>,
}

unsafe impl Send for ContextStorage {}
unsafe impl Sync for ContextStorage {}

pub(crate) struct PollerStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) sockets: Mutex<HashMap<usize, PollerSocketRegistration>>,
    // Poller is Send but not Sync, so a wait call can reuse this ABI scratch
    // buffer without adding a mutex to every hot-path wait.
    pub(crate) raw_events: UnsafeCell<Vec<ffi::zlink_poller_event_t>>,
}

unsafe impl Send for PollerStorage {}

pub(crate) struct TimerStorage {
    pub(crate) handle: *mut c_void,
}

unsafe impl Send for TimerStorage {}
unsafe impl Sync for TimerStorage {}

pub(crate) struct MonitorStorage {
    pub(crate) handle: *mut c_void,
}

unsafe impl Send for MonitorStorage {}

pub(crate) struct SocketStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) completion_owner: Option<Arc<super::CompletionOwner>>,
    pub(crate) routed_handle: Option<Arc<super::RoutedHandle>>,
    pub(crate) reply_owner: Option<Arc<super::RouterOwnerTag>>,
}

unsafe impl Send for SocketStorage {}
