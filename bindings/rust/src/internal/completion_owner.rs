// SPDX-License-Identifier: MPL-2.0

//! Socket-local ownership of the Core completion queue.
//!
//! One owner drains a socket at a time.  The binding runtime owns unregistered
//! sockets; a public poller atomically takes that responsibility while it has a
//! `POLLCOMPLETION` registration.  Entries are registered by stable
//! `user_context` before the native FINAL call so an eager drain can capture a
//! completion before the submit call publishes its completion id.

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::task::{Poll, Waker};
use std::thread::JoinHandle;

use crate::error::{
    ConfigError, ConfigResult, RecvError, RequestError, RequestResult, SubmitError, SubmitResult,
};
use crate::ffi;
use crate::message::Message;
use crate::native_errors::{check_config_rc, check_recv_rc, request_error_from_result};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CompletionKind {
    Send,
    Request,
}

enum CompletionOutcome {
    Send(Result<(), SubmitError>),
    Request(Result<Vec<Message>, RequestError>),
}

struct EntryState {
    completion_id: u64,
    published: bool,
    captured: bool,
    settled: bool,
    detached: bool,
    outcome: Option<CompletionOutcome>,
    waker: Option<Waker>,
}

/// One provisional send or request operation.
pub(crate) struct CompletionEntry {
    kind: CompletionKind,
    state: Mutex<EntryState>,
    changed: Condvar,
}

impl CompletionEntry {
    pub(crate) fn new(kind: CompletionKind) -> Arc<Self> {
        Arc::new(Self {
            kind,
            state: Mutex::new(EntryState {
                completion_id: 0,
                published: false,
                captured: false,
                settled: false,
                detached: false,
                outcome: None,
                waker: None,
            }),
            changed: Condvar::new(),
        })
    }

    pub(crate) fn publish(&self, completion_id: u64) {
        let waker = {
            let mut state = self.state.lock().expect("completion entry");
            state.completion_id = completion_id;
            state.published = true;
            Self::settle_if_joined(&mut state)
        };
        self.changed.notify_all();
        if let Some(waker) = waker {
            waker.wake();
        }
    }

    pub(crate) fn capture_inline_send(&self) {
        let waker = {
            let mut state = self.state.lock().expect("completion entry");
            state.captured = true;
            if !state.detached {
                state.outcome = Some(CompletionOutcome::Send(Ok(())));
            }
            Self::settle_if_joined(&mut state)
        };
        self.changed.notify_all();
        if let Some(waker) = waker {
            waker.wake();
        }
    }

    fn capture(&self, completion: &mut ffi::zlink_completion_t) {
        let outcome = completion_outcome(self.kind, completion);
        let waker = {
            let mut state = self.state.lock().expect("completion entry");
            if state.captured {
                return;
            }
            state.captured = true;
            if !state.detached {
                state.outcome = Some(outcome);
            }
            Self::settle_if_joined(&mut state)
        };
        self.changed.notify_all();
        if let Some(waker) = waker {
            waker.wake();
        }
    }

    fn settle_if_joined(state: &mut EntryState) -> Option<Waker> {
        if !state.published || !state.captured || state.settled {
            return None;
        }
        state.settled = true;
        state.waker.take()
    }

    pub(crate) fn poll_send(&self, waker: &Waker) -> Poll<Result<(), SubmitError>> {
        let mut state = self.state.lock().expect("completion entry");
        if state.settled {
            return match state.outcome.take().expect("live send outcome") {
                CompletionOutcome::Send(outcome) => Poll::Ready(outcome),
                CompletionOutcome::Request(_) => unreachable!("request outcome in send entry"),
            };
        }
        if state.waker.as_ref().is_none_or(|old| !old.will_wake(waker)) {
            state.waker = Some(waker.clone());
        }
        Poll::Pending
    }

    pub(crate) fn poll_request(&self, waker: &Waker) -> Poll<Result<Vec<Message>, RequestError>> {
        let mut state = self.state.lock().expect("completion entry");
        if state.settled {
            return match state.outcome.take().expect("live request outcome") {
                CompletionOutcome::Request(outcome) => Poll::Ready(outcome),
                CompletionOutcome::Send(_) => unreachable!("send outcome in request entry"),
            };
        }
        if state.waker.as_ref().is_none_or(|old| !old.will_wake(waker)) {
            state.waker = Some(waker.clone());
        }
        Poll::Pending
    }

    pub(crate) fn wait_request(&self) -> Result<Vec<Message>, RequestError> {
        let mut state = self.state.lock().expect("completion entry");
        while !state.settled {
            state = self.changed.wait(state).expect("completion entry");
        }
        match state.outcome.take().expect("blocking request outcome") {
            CompletionOutcome::Request(outcome) => outcome,
            CompletionOutcome::Send(_) => unreachable!("send outcome in request entry"),
        }
    }

    fn wait_settled(&self) {
        let mut state = self.state.lock().expect("completion entry");
        while !state.settled {
            state = self.changed.wait(state).expect("completion entry");
        }
    }

    /// Detaches only the Rust waiter.  Core ownership remains registered until
    /// a late completion or socket lifecycle cleanup removes the entry.
    pub(crate) fn detach(&self) {
        let mut state = self.state.lock().expect("completion entry");
        state.detached = true;
        state.outcome = None;
        state.waker = None;
    }

    fn shutdown(&self) {
        let waker = {
            let mut state = self.state.lock().expect("completion entry");
            if state.settled {
                return;
            }
            state.published = true;
            state.captured = true;
            if !state.detached {
                state.outcome = Some(match self.kind {
                    CompletionKind::Send => CompletionOutcome::Send(Err(SubmitError::new(
                        SubmitResult::NotAdmitted,
                        libc::ESHUTDOWN,
                    ))),
                    CompletionKind::Request => CompletionOutcome::Request(Err(RequestError::new(
                        RequestResult::Terminated,
                        libc::ESHUTDOWN,
                    ))),
                });
            }
            Self::settle_if_joined(&mut state)
        };
        self.changed.notify_all();
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

struct OwnerState {
    entries: HashMap<usize, Arc<CompletionEntry>>,
    public_owner: Option<usize>,
    runtime_poller: usize,
    runtime_thread: Option<JoinHandle<()>>,
    runtime_stop: bool,
    shutdown: bool,
}

/// The single drain owner and provisional registry for one native socket.
pub(crate) struct CompletionOwner {
    socket: usize,
    state: Mutex<OwnerState>,
}

unsafe impl Send for CompletionOwner {}
unsafe impl Sync for CompletionOwner {}

impl CompletionOwner {
    pub(crate) fn new(socket: *mut c_void) -> Arc<Self> {
        Arc::new(Self {
            socket: socket as usize,
            state: Mutex::new(OwnerState {
                entries: HashMap::new(),
                public_owner: None,
                runtime_poller: 0,
                runtime_thread: None,
                runtime_stop: false,
                shutdown: false,
            }),
        })
    }

    pub(crate) fn register(
        self: &Arc<Self>,
        kind: CompletionKind,
    ) -> Result<(Arc<CompletionEntry>, *mut c_void), SubmitError> {
        let entry = CompletionEntry::new(kind);
        let key = Arc::as_ptr(&entry) as usize;
        let mut state = self.state.lock().expect("completion owner");
        if state.shutdown {
            return Err(SubmitError::new(
                SubmitResult::InvalidState,
                libc::ESHUTDOWN,
            ));
        }
        state.entries.insert(key, Arc::clone(&entry));
        if state.public_owner.is_none() {
            if let Err(error) = self.start_runtime_locked(&mut state) {
                state.entries.remove(&key);
                return Err(SubmitError::new(
                    SubmitResult::InternalError,
                    error.native_errno(),
                ));
            }
        }
        Ok((entry, key as *mut c_void))
    }

    pub(crate) fn unregister(&self, user_context: *mut c_void) {
        self.state
            .lock()
            .expect("completion owner")
            .entries
            .remove(&(user_context as usize));
    }

    pub(crate) fn drain(&self, wait_for_publish: bool) -> Result<usize, RecvError> {
        let mut processed = 0;
        loop {
            let mut completion = ffi::zlink_completion_t::empty();
            let rc = unsafe {
                ffi::zlink_completion_recv(
                    self.socket as *mut c_void,
                    &mut completion,
                    ffi::ZLINK_RECV_DONTWAIT,
                )
            };
            if rc == crate::error::RecvResult::NoData as i32 {
                break;
            }
            if rc != crate::error::RecvResult::Ok as i32 {
                return Err(check_recv_rc(rc).expect_err("failed completion receive"));
            }

            let entry = self
                .state
                .lock()
                .expect("completion owner")
                .entries
                .get(&(completion.user_context as usize))
                .cloned();
            if let Some(entry) = entry {
                entry.capture(&mut completion);
                if wait_for_publish {
                    entry.wait_settled();
                }
                self.unregister(Arc::as_ptr(&entry) as *mut c_void);
            } else {
                unsafe { ffi::zlink_completion_close(&mut completion) };
            }
            processed += 1;
        }
        Ok(processed)
    }

    pub(crate) fn transfer_to_public(
        self: &Arc<Self>,
        poller_owner: usize,
    ) -> Result<(), ConfigError> {
        let mut state = self.state.lock().expect("completion owner");
        if state.shutdown {
            return Err(ConfigError::new(
                ConfigResult::InvalidState,
                libc::ESHUTDOWN,
            ));
        }
        if state
            .public_owner
            .is_some_and(|owner| owner != poller_owner)
        {
            return Err(ConfigError::new(ConfigResult::InvalidState, libc::EBUSY));
        }
        if state.public_owner.is_none() {
            state = self.stop_runtime_locked(state);
            state.public_owner = Some(poller_owner);
        }
        Ok(())
    }

    pub(crate) fn transfer_to_runtime(self: &Arc<Self>, poller_owner: usize) {
        let mut state = self.state.lock().expect("completion owner");
        if state.public_owner != Some(poller_owner) {
            return;
        }
        state.public_owner = None;
        if !state.entries.is_empty() && !state.shutdown {
            let _ = self.start_runtime_locked(&mut state);
        }
    }

    pub(crate) fn shutdown(self: &Arc<Self>) {
        let entries = {
            let mut state = self.state.lock().expect("completion owner");
            if state.shutdown {
                return;
            }
            state.shutdown = true;
            state = self.stop_runtime_locked(state);
            std::mem::take(&mut state.entries)
        };
        for entry in entries.into_values() {
            entry.shutdown();
        }
    }

    fn start_runtime_locked(self: &Arc<Self>, state: &mut OwnerState) -> Result<(), ConfigError> {
        if state.runtime_thread.is_some() || state.shutdown {
            return Ok(());
        }
        let poller = unsafe { ffi::zlink_poller_new() };
        if poller.is_null() {
            return Err(ConfigError::new(ConfigResult::InternalError, unsafe {
                ffi::zlink_errno()
            }));
        }
        if let Err(error) = check_config_rc(unsafe {
            ffi::zlink_poller_add(
                poller,
                self.socket as *mut c_void,
                std::ptr::null_mut(),
                crate::POLLCOMPLETION,
            )
        }) {
            let mut raw = poller;
            unsafe { ffi::zlink_poller_destroy(&mut raw) };
            return Err(error);
        }
        state.runtime_poller = poller as usize;
        state.runtime_stop = false;
        let owner = Arc::clone(self);
        state.runtime_thread = Some(std::thread::spawn(move || owner.runtime_loop()));
        Ok(())
    }

    fn stop_runtime_locked<'a>(
        &'a self,
        mut state: MutexGuard<'a, OwnerState>,
    ) -> MutexGuard<'a, OwnerState> {
        state.runtime_stop = true;
        let thread = state.runtime_thread.take();
        let poller = std::mem::take(&mut state.runtime_poller);
        drop(state);
        if let Some(thread) = thread {
            if thread.thread().id() == std::thread::current().id() {
                drop(thread);
            } else {
                let _ = thread.join();
            }
        }
        if poller != 0 {
            let mut raw = poller as *mut c_void;
            unsafe { ffi::zlink_poller_destroy(&mut raw) };
        }
        self.state.lock().expect("completion owner")
    }

    fn runtime_loop(self: Arc<Self>) {
        loop {
            let poller = {
                let state = self.state.lock().expect("completion owner");
                if state.runtime_stop || state.shutdown {
                    return;
                }
                state.runtime_poller as *mut c_void
            };
            let mut event = ffi::zlink_poller_event_t::empty();
            let rc =
                unsafe { ffi::zlink_poller_wait(poller, &mut event, 1, 25, std::ptr::null_mut()) };
            match rc.cmp(&0) {
                std::cmp::Ordering::Greater => {
                    if self.drain(false).is_err() {
                        return;
                    }
                }
                std::cmp::Ordering::Less => {
                    let errno = unsafe { ffi::zlink_errno() };
                    if errno != libc::EINTR && errno != libc::EAGAIN {
                        return;
                    }
                }
                std::cmp::Ordering::Equal => {}
            }
        }
    }
}

fn completion_outcome(
    kind: CompletionKind,
    completion: &mut ffi::zlink_completion_t,
) -> CompletionOutcome {
    struct CloseGuard(*mut ffi::zlink_completion_t);
    impl Drop for CloseGuard {
        fn drop(&mut self) {
            unsafe { ffi::zlink_completion_close(self.0) };
        }
    }
    let _guard = CloseGuard(completion);

    match kind {
        CompletionKind::Send => {
            let outcome = if completion.kind == ffi::zlink_completion_kind_t::ZLINK_COMPLETION_SEND
                && completion.send_result == ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED
            {
                Ok(())
            } else {
                let errno = if completion.send_terminal_errno == 0 {
                    libc::EIO
                } else {
                    completion.send_terminal_errno
                };
                Err(SubmitError::new(SubmitResult::NotAdmitted, errno))
            };
            CompletionOutcome::Send(outcome)
        }
        CompletionKind::Request => {
            let outcome = if completion.kind
                != ffi::zlink_completion_kind_t::ZLINK_COMPLETION_REQUEST
            {
                Err(RequestError::new(
                    RequestResult::InternalError,
                    libc::EPROTO,
                ))
            } else if completion.request_result != ffi::zlink_request_result_t::ZLINK_REQUEST_OK {
                Err(request_error_from_result(request_result_from_native(
                    completion.request_result,
                )))
            } else {
                Ok(take_completion_parts(
                    completion.reply_parts,
                    completion.reply_part_count,
                ))
            };
            CompletionOutcome::Request(outcome)
        }
    }
}

fn take_completion_parts(parts: *mut ffi::zlink_msg_t, count: usize) -> Vec<Message> {
    let mut out = Vec::with_capacity(count);
    if parts.is_null() {
        return out;
    }
    for index in 0..count {
        unsafe {
            let mut native = std::mem::MaybeUninit::<ffi::zlink_msg_t>::uninit();
            ffi::zlink_msg_init(native.as_mut_ptr());
            ffi::zlink_msg_move(native.as_mut_ptr(), parts.add(index));
            out.push(Message::from_raw(native.assume_init()));
        }
    }
    out
}

fn request_result_from_native(result: ffi::zlink_request_result_t) -> RequestResult {
    match result {
        ffi::zlink_request_result_t::ZLINK_REQUEST_OK => RequestResult::Ok,
        ffi::zlink_request_result_t::ZLINK_REQUEST_TIMED_OUT => RequestResult::TimedOut,
        ffi::zlink_request_result_t::ZLINK_REQUEST_NOT_FOUND => RequestResult::NotFound,
        ffi::zlink_request_result_t::ZLINK_REQUEST_TERMINATED => RequestResult::Terminated,
        ffi::zlink_request_result_t::ZLINK_REQUEST_PROTOCOL_ERROR => RequestResult::ProtocolError,
        ffi::zlink_request_result_t::ZLINK_REQUEST_INTERNAL_ERROR => RequestResult::InternalError,
        ffi::zlink_request_result_t::ZLINK_REQUEST_REJECTED => RequestResult::Rejected,
        ffi::zlink_request_result_t::ZLINK_REQUEST_CONFLICT => RequestResult::Conflict,
        ffi::zlink_request_result_t::ZLINK_REQUEST_BUSY => RequestResult::Busy,
        ffi::zlink_request_result_t::ZLINK_REQUEST_NOT_CONNECTED => RequestResult::NotConnected,
        ffi::zlink_request_result_t::ZLINK_REQUEST_INVALID_ARGUMENT => {
            RequestResult::InvalidArgument
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_INVALID_STATE => RequestResult::InvalidState,
        ffi::zlink_request_result_t::ZLINK_REQUEST_NOT_SUPPORTED => RequestResult::NotSupported,
        ffi::zlink_request_result_t::ZLINK_REQUEST_BACKPRESSURED => RequestResult::Backpressured,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::task::{Wake, Waker};

    struct NoopWake;
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    #[test]
    fn capture_before_publish_joins_exactly_once() {
        let entry = CompletionEntry::new(CompletionKind::Send);
        let mut completion = ffi::zlink_completion_t::empty();
        completion.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_SEND;
        completion.completion_id = 41;
        completion.send_result = ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED;
        entry.capture(&mut completion);

        let waker = Waker::from(Arc::new(NoopWake));
        assert!(entry.poll_send(&waker).is_pending());
        entry.publish(41);
        assert!(matches!(entry.poll_send(&waker), Poll::Ready(Ok(()))));
    }

    #[test]
    fn detached_late_completion_discards_outcome_after_cleanup() {
        let entry = CompletionEntry::new(CompletionKind::Request);
        entry.detach();
        let mut completion = ffi::zlink_completion_t::empty();
        completion.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_REQUEST;
        completion.completion_id = 52;
        completion.request_result = ffi::zlink_request_result_t::ZLINK_REQUEST_TIMED_OUT;
        entry.capture(&mut completion);
        entry.publish(52);

        let state = entry.state.lock().expect("completion state");
        assert!(state.settled);
        assert!(state.outcome.is_none());
    }
}
