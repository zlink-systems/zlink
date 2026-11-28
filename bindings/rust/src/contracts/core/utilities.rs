use std::ffi::c_void;

/// A high-resolution elapsed-time stopwatch.
pub struct Stopwatch {
    pub(crate) handle: *mut c_void,
}

/// A thread-safe integer counter.
pub struct AtomicCounter {
    pub(crate) handle: *mut c_void,
}

/// A running background thread created by the zlink runtime.
pub struct Thread {
    pub(crate) handle: *mut c_void,
    pub(crate) state: *mut c_void,
}
