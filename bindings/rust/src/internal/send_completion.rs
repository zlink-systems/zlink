// SPDX-License-Identifier: MPL-2.0

//! Per-socket Core send-completion delivery.
//!
//! The 0.13.0 contract (`bindings/doc/spec/async-coroutine-policy.ko.md`, 3rd
//! revision; `doc/plan/core-send-completion-design.ko.md`) makes the binding a
//! pure Core wrapper: every HWM wait, retry and deadline belongs to Core. This
//! module owns nothing but the map from one in-flight operation to the waker of
//! the `Future` that awaits it. The registered
//! `zlink_send_complete_handler` only stores the result and wakes; it never
//! submits (Core answers a submit from inside a completion with `EDEADLK`).

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::{Arc, Mutex};
use std::task::{Poll, Waker};

use crate::error::{SubmitError, SubmitResult};
use crate::ffi;
use crate::native_errors::submit_error_from_errno;

/// One in-flight `zlink_send_async` operation.
///
/// The slot is created and registered *before* the native submit, because Core
/// may run the completion inline inside `zlink_send_async` when the record is
/// admitted immediately. The registry keeps its own `Arc` until the completion
/// arrives, so a dropped `Future` never frees state Core still points at.
pub(crate) struct SendCompletionSlot {
    state: Mutex<SlotState>,
}

struct SlotState {
    outcome: Option<Result<(), SubmitError>>,
    waker: Option<Waker>,
    op_id: ffi::zlink_send_op_id_t,
    detached: bool,
}

impl SendCompletionSlot {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(SlotState {
                outcome: None,
                waker: None,
                op_id: 0,
                detached: false,
            }),
        })
    }

    /// Records the Core operation id once `zlink_send_async` has returned.
    /// Returns `false` when the completion already ran inline, in which case
    /// there is nothing left to cancel.
    pub(crate) fn arm(&self, op_id: ffi::zlink_send_op_id_t) -> bool {
        let mut state = self.state.lock().expect("send completion state");
        if state.outcome.is_some() {
            return false;
        }
        state.op_id = op_id;
        true
    }

    pub(crate) fn op_id(&self) -> ffi::zlink_send_op_id_t {
        self.state.lock().expect("send completion state").op_id
    }

    pub(crate) fn poll(&self, waker: &Waker) -> Poll<Result<(), SubmitError>> {
        let mut state = self.state.lock().expect("send completion state");
        if let Some(outcome) = state.outcome.take() {
            return Poll::Ready(outcome);
        }
        if state
            .waker
            .as_ref()
            .is_none_or(|registered| !registered.will_wake(waker))
        {
            state.waker = Some(waker.clone());
        }
        Poll::Pending
    }

    /// Drops the consumer side. The registry entry stays until Core delivers
    /// the single guaranteed completion.
    pub(crate) fn detach(&self) {
        let mut state = self.state.lock().expect("send completion state");
        state.detached = true;
        state.waker = None;
    }

    fn complete(&self, outcome: Result<(), SubmitError>) {
        let waker = {
            let mut state = self.state.lock().expect("send completion state");
            if state.outcome.is_some() {
                return;
            }
            if state.detached {
                return;
            }
            state.outcome = Some(outcome);
            state.waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

/// Socket-scoped registry of in-flight send operations.
pub(crate) struct SendCompletions {
    slots: Mutex<HashMap<usize, Arc<SendCompletionSlot>>>,
}

impl SendCompletions {
    pub(crate) fn new() -> Arc<Self> {
        Arc::new(Self {
            slots: Mutex::new(HashMap::new()),
        })
    }

    /// Registers one slot and returns it together with the `userdata` key that
    /// must be handed to `zlink_send_async`.
    pub(crate) fn register(&self) -> (Arc<SendCompletionSlot>, *mut c_void) {
        let slot = SendCompletionSlot::new();
        let key = Arc::as_ptr(&slot) as usize;
        self.slots
            .lock()
            .expect("send completion registry")
            .insert(key, Arc::clone(&slot));
        (slot, key as *mut c_void)
    }

    /// Removes a registration whose submit never reached `ZLINK_SUBMIT_OK`, so
    /// no completion will ever arrive for it.
    pub(crate) fn discard(&self, key: *mut c_void) {
        self.slots
            .lock()
            .expect("send completion registry")
            .remove(&(key as usize));
    }

    fn deliver(&self, event: &ffi::zlink_send_complete_event_t) {
        let slot = self
            .slots
            .lock()
            .expect("send completion registry")
            .remove(&(event.userdata as usize));
        let Some(slot) = slot else {
            return;
        };
        slot.complete(outcome_from_event(event));
    }

    /// Fails every still-registered operation after the native handle is gone.
    /// Core completes each in-flight operation exactly once while closing, so
    /// this only covers the operations whose completion could no longer be
    /// dispatched to this binding.
    pub(crate) fn drain_terminal(&self, errno: i32) {
        let slots: Vec<_> = self
            .slots
            .lock()
            .expect("send completion registry")
            .drain()
            .map(|(_, slot)| slot)
            .collect();
        for slot in slots {
            slot.complete(Err(submit_error_from_errno(errno)));
        }
    }
}

fn outcome_from_event(event: &ffi::zlink_send_complete_event_t) -> Result<(), SubmitError> {
    if event.result == ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED {
        return Ok(());
    }
    let errno = if event.terminal_errno != 0 {
        event.terminal_errno
    } else if event.result == ffi::zlink_send_complete_result_t::ZLINK_SEND_TIMED_OUT {
        libc::ETIMEDOUT
    } else {
        libc::EIO
    };
    // Rust has no second result domain for a Core operation that reached the
    // completion lane without being admitted; `NotAdmitted` is that outcome and
    // the Core errno keeps the timeout / cancel / close / route detail.
    Err(SubmitError::new(SubmitResult::NotAdmitted, errno))
}

/// Core send-completion trampoline. Delivery only: it stores the result and
/// wakes the waiting `Future`, and never calls back into Core.
pub(crate) unsafe extern "C" fn send_complete_trampoline(
    _subject: *mut c_void,
    event: *const ffi::zlink_send_complete_event_t,
    userdata: *mut c_void,
) {
    if event.is_null() {
        return;
    }
    let event = unsafe { &*event };
    let _ = unsafe {
        super::CallbackBox::invoke::<Arc<SendCompletions>, _>(userdata, |completions| {
            completions.deliver(event)
        })
    };
}
