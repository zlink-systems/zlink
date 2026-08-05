use std::collections::VecDeque;
use std::ffi::c_void;
use std::sync::{Condvar, Mutex, OnceLock};
use std::thread;
use std::time::Duration;

use crate::ffi;

use super::CallbackBox;

#[derive(Clone, Copy)]
pub(crate) enum DeferredCloseKind {
    Socket,
    Monitor,
    Timer,
}

enum DeferredCleanupAction {
    ReleaseCallbacks,
    CloseNative {
        kind: DeferredCloseKind,
        handle: *mut c_void,
    },
}

struct DeferredCleanup {
    action: DeferredCleanupAction,
    callbacks: Vec<CallbackBox>,
}

unsafe impl Send for DeferredCleanup {}

static DEFERRED_CLEANUP_QUEUE: OnceLock<(Mutex<VecDeque<DeferredCleanup>>, Condvar)> =
    OnceLock::new();
static DEFERRED_CLEANUP_WORKER: OnceLock<bool> = OnceLock::new();

fn deferred_cleanup_queue() -> &'static (Mutex<VecDeque<DeferredCleanup>>, Condvar) {
    DEFERRED_CLEANUP_QUEUE.get_or_init(|| (Mutex::new(VecDeque::new()), Condvar::new()))
}

fn start_deferred_cleanup_worker() -> bool {
    *DEFERRED_CLEANUP_WORKER.get_or_init(|| {
        thread::Builder::new()
            .name("zlink-rust-cleanup".to_owned())
            .spawn(deferred_cleanup_loop)
            .is_ok()
    })
}

fn enqueue_deferred_cleanup(cleanup: DeferredCleanup) {
    if !start_deferred_cleanup_worker() {
        // Drop cannot return an allocation or thread creation failure. Keep the
        // native handle and callback userdata alive rather than free storage
        // that Core may still reference.
        std::mem::forget(cleanup);
        return;
    }
    let queue = deferred_cleanup_queue();
    queue
        .0
        .lock()
        .expect("deferred cleanup queue")
        .push_back(cleanup);
    queue.1.notify_one();
}

fn deferred_cleanup_loop() {
    loop {
        let cleanup = {
            let queue = deferred_cleanup_queue();
            let mut pending = queue.0.lock().expect("deferred cleanup queue");
            while pending.is_empty() {
                pending = queue.1.wait(pending).expect("deferred cleanup wait");
            }
            pending.pop_front().expect("deferred cleanup item")
        };

        if let Some(cleanup) = try_deferred_cleanup(cleanup) {
            let queue = deferred_cleanup_queue();
            let guard = queue.0.lock().expect("deferred cleanup queue");
            let (mut pending, _) = queue
                .1
                .wait_timeout(guard, Duration::from_millis(10))
                .expect("deferred cleanup backoff wait");
            pending.push_back(cleanup);
            queue.1.notify_one();
        }
    }
}

fn try_deferred_cleanup(mut cleanup: DeferredCleanup) -> Option<DeferredCleanup> {
    if !cleanup.callbacks.iter().all(CallbackBox::is_idle) {
        return Some(cleanup);
    }

    match cleanup.action {
        DeferredCleanupAction::ReleaseCallbacks => None,
        DeferredCleanupAction::CloseNative { kind, handle } => {
            let rc = unsafe {
                match kind {
                    DeferredCloseKind::Socket => ffi::zlink_close(handle),
                    DeferredCloseKind::Monitor => {
                        let mut handle = handle;
                        ffi::zlink_monitor_close(&mut handle)
                    }
                    DeferredCloseKind::Timer => {
                        let mut handle = handle;
                        ffi::zlink_timer_destroy(&mut handle)
                    }
                }
            };
            if rc == crate::results::CloseResult::Ok as i32 {
                cleanup.action = DeferredCleanupAction::ReleaseCallbacks;
                if cleanup.callbacks.iter().all(CallbackBox::is_idle) {
                    None
                } else {
                    Some(cleanup)
                }
            } else if rc == crate::results::CloseResult::Busy as i32 {
                Some(cleanup)
            } else {
                // Only BUSY is retryable by the Core close contract. Preserve
                // storage on a terminal failure without monopolizing the
                // shared cleanup worker.
                std::mem::forget(cleanup);
                None
            }
        }
    }
}

pub(crate) fn release_callbacks(mut callbacks: Vec<CallbackBox>) {
    for callback in &callbacks {
        callback.set_closing(true);
    }
    if callbacks.iter().all(CallbackBox::is_idle) {
        callbacks.clear();
    } else {
        enqueue_deferred_cleanup(DeferredCleanup {
            action: DeferredCleanupAction::ReleaseCallbacks,
            callbacks,
        });
    }
}

pub(crate) fn defer_native_close(
    kind: DeferredCloseKind,
    handle: *mut c_void,
    callbacks: Vec<CallbackBox>,
) {
    for callback in &callbacks {
        callback.set_closing(true);
    }
    enqueue_deferred_cleanup(DeferredCleanup {
        action: DeferredCleanupAction::CloseNative { kind, handle },
        callbacks,
    });
}
