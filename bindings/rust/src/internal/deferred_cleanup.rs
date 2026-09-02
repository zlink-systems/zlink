use std::collections::VecDeque;
use std::ffi::c_void;
use std::sync::{Condvar, Mutex, OnceLock};
use std::thread;
use std::time::Duration;

use crate::ffi;

#[derive(Clone, Copy)]
pub(crate) enum DeferredCloseKind {
    Socket,
    Monitor,
    Timer,
}

struct DeferredClose {
    kind: DeferredCloseKind,
    handle: *mut c_void,
}
unsafe impl Send for DeferredClose {}

static QUEUE: OnceLock<(Mutex<VecDeque<DeferredClose>>, Condvar)> = OnceLock::new();
static WORKER: OnceLock<bool> = OnceLock::new();

fn queue() -> &'static (Mutex<VecDeque<DeferredClose>>, Condvar) {
    QUEUE.get_or_init(|| (Mutex::new(VecDeque::new()), Condvar::new()))
}

fn start_worker() -> bool {
    *WORKER.get_or_init(|| {
        thread::Builder::new()
            .name("zlink-rust-cleanup".to_owned())
            .spawn(cleanup_loop)
            .is_ok()
    })
}

pub(crate) fn defer_native_close(kind: DeferredCloseKind, handle: *mut c_void) {
    let close = DeferredClose { kind, handle };
    if !start_worker() {
        return;
    }
    let queue = queue();
    queue
        .0
        .lock()
        .expect("deferred cleanup queue")
        .push_back(close);
    queue.1.notify_one();
}

fn cleanup_loop() {
    loop {
        let close = {
            let queue = queue();
            let mut pending = queue.0.lock().expect("deferred cleanup queue");
            while pending.is_empty() {
                pending = queue.1.wait(pending).expect("deferred cleanup wait");
            }
            pending.pop_front().expect("deferred close")
        };
        if let Some(close) = try_close(close) {
            let queue = queue();
            let guard = queue.0.lock().expect("deferred cleanup queue");
            let (mut pending, _) = queue
                .1
                .wait_timeout(guard, Duration::from_millis(10))
                .expect("deferred cleanup backoff");
            pending.push_back(close);
            queue.1.notify_one();
        }
    }
}

fn try_close(close: DeferredClose) -> Option<DeferredClose> {
    let rc = unsafe {
        match close.kind {
            DeferredCloseKind::Socket => ffi::zlink_close(close.handle),
            DeferredCloseKind::Monitor => {
                let mut handle = close.handle;
                ffi::zlink_monitor_close(&mut handle)
            }
            DeferredCloseKind::Timer => {
                let mut handle = close.handle;
                ffi::zlink_timer_destroy(&mut handle)
            }
        }
    };
    if rc == crate::CloseResult::Ok as i32 {
        None
    } else if rc == crate::CloseResult::Busy as i32 {
        Some(close)
    } else {
        None
    }
}
