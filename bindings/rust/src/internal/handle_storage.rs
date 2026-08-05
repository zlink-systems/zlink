use std::cell::UnsafeCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::Mutex;

use crate::ffi;

use super::CallbackBox;

pub(crate) struct ContextStorage {
    pub(crate) handle: *mut c_void,
    /// Cached thread name prefix (write-only in the C API; readable from cache).
    pub(crate) thread_name_prefix: Mutex<String>,
}

unsafe impl Send for ContextStorage {}
unsafe impl Sync for ContextStorage {}

pub(crate) struct PollerStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) sockets: Mutex<HashMap<usize, i16>>,
    // Poller is Send but not Sync, so a wait call can reuse this ABI scratch
    // buffer without adding a mutex to every hot-path wait.
    pub(crate) raw_events: UnsafeCell<Vec<ffi::zlink_poller_event_t>>,
}

unsafe impl Send for PollerStorage {}

pub(crate) struct TimerStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) callback: Mutex<Option<CallbackBox>>,
}

unsafe impl Send for TimerStorage {}
unsafe impl Sync for TimerStorage {}

pub(crate) struct MonitorStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) callback: Option<CallbackBox>,
}

unsafe impl Send for MonitorStorage {}

pub(crate) struct SocketStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) send_ready_cb: Option<CallbackBox>,
    pub(crate) packet_cb: Option<CallbackBox>,
}

unsafe impl Send for SocketStorage {}
