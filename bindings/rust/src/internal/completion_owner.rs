// SPDX-License-Identifier: MPL-2.0

//! Socket-local ownership of the Core completion queue.
//!
//! One owner drains a socket at a time.  The binding runtime owns unregistered
//! sockets and drains them from one lazily started reactor thread that blocks
//! in a native poller; a public poller atomically takes that responsibility
//! while it has a `POLLCOMPLETION` registration. REQUEST entries are
//! registered before the native FINAL call and may move from WRITABLE waits to
//! the final REQUEST completion. SEND entries are registered only when Core
//! hands out a wait token: a WRITABLE record that a concurrent drain pulls
//! before that registration lands is parked by its opaque context and replayed
//! when the registration arrives.

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, RwLock};
use std::task::{Poll, Waker};
use std::thread::JoinHandle;

use crate::error::{
    ConfigError, ConfigResult, RecvError, RequestError, RequestResult, SubmitError, SubmitResult,
};
use crate::ffi;
use crate::message::Message;
use crate::native_errors::{
    check_config_rc, check_recv_rc, request_error_from_result, send_terminal_error,
    submit_error_from_errno,
};

/// Reactor thread wait bound. The thread wakes early on any completion record;
/// this only bounds how long a stop or ownership transfer waits for it.
const REACTOR_WAIT_MS: i64 = 25;
/// Idle wait rounds before an unused reactor thread retires itself.
const REACTOR_IDLE_ROUNDS: u32 = 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CompletionEntryKind {
    SendRetry,
    Request,
}

enum CompletionOutcome {
    Writable {
        completion_id: u64,
        result: Result<(), SubmitError>,
    },
    Request(Result<Vec<Message>, RequestError>),
}

struct CaptureResult {
    detached: bool,
    waker: Option<Waker>,
}

/// A WRITABLE record pulled before its SEND registration landed.
#[derive(Clone, Copy)]
struct ParkedWritable {
    completion_id: u64,
    send_result: ffi::zlink_send_complete_result_t,
    send_terminal_errno: i32,
}

struct EntryState {
    completion_id: u64,
    published: bool,
    captured: bool,
    settled: bool,
    detached: bool,
    owner_shutdown: bool,
    awaiting_writable: bool,
    outcome: Option<CompletionOutcome>,
    waker: Option<Waker>,
}

/// One provisional send or request operation.
pub(crate) struct CompletionEntry {
    kind: CompletionEntryKind,
    state: Mutex<EntryState>,
    changed: Condvar,
}

impl CompletionEntry {
    pub(crate) fn new(kind: CompletionEntryKind) -> Arc<Self> {
        Arc::new(Self {
            kind,
            state: Mutex::new(EntryState {
                completion_id: 0,
                published: false,
                captured: false,
                settled: false,
                detached: false,
                owner_shutdown: false,
                awaiting_writable: kind == CompletionEntryKind::SendRetry,
                outcome: None,
                waker: None,
            }),
            changed: Condvar::new(),
        })
    }

    pub(crate) fn kind(&self) -> CompletionEntryKind {
        self.kind
    }

    fn publish(&self, completion_id: u64, awaiting_writable: bool) {
        let waker = {
            let mut state = self.state.lock().expect("completion entry");
            state.completion_id = completion_id;
            state.published = true;
            state.awaiting_writable = awaiting_writable;
            if !state.owner_shutdown
                && matches!(
                    state.outcome.as_ref(),
                    Some(CompletionOutcome::Writable {
                        completion_id: captured_id,
                        ..
                    }) if *captured_id != completion_id
                )
            {
                // A record for another token must never resume this packet.
                // Keep the stable context registered for the expected record.
                state.captured = false;
                state.outcome = None;
            }
            Self::settle_if_joined(&mut state)
        };
        self.changed.notify_all();
        if let Some(waker) = waker {
            waker.wake();
        }
    }

    pub(crate) fn publish_writable(&self, completion_id: u64) {
        self.publish(completion_id, true);
    }

    pub(crate) fn publish_request(&self, completion_id: u64) {
        self.publish(completion_id, false);
    }

    fn capture(&self, completion: &mut ffi::zlink_completion_t) -> CaptureResult {
        let outcome = completion_outcome(self.kind, completion);
        self.capture_outcome(outcome)
    }

    fn capture_parked(&self, parked: &ParkedWritable) -> CaptureResult {
        let outcome = writable_outcome(
            ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE,
            parked.completion_id,
            parked.send_result,
            parked.send_terminal_errno,
        );
        self.capture_outcome(outcome)
    }

    fn capture_outcome(&self, outcome: CompletionOutcome) -> CaptureResult {
        let (waker, detached) = {
            let mut state = self.state.lock().expect("completion entry");
            if state.captured {
                return CaptureResult {
                    detached: state.detached,
                    waker: None,
                };
            }
            if matches!(
                &outcome,
                CompletionOutcome::Writable {
                    completion_id: captured_id,
                    ..
                } if state.published && *captured_id != state.completion_id
            ) {
                // Ignore a mismatched token while retaining the sink for the
                // one Core promised. The opaque context must not be reused
                // until that expected token is retired.
                return CaptureResult {
                    detached: false,
                    waker: None,
                };
            }
            state.captured = true;
            if !state.detached {
                state.outcome = Some(outcome);
            }
            (Self::settle_if_joined(&mut state), state.detached)
        };
        self.changed.notify_all();
        CaptureResult { detached, waker }
    }

    fn settle_if_joined(state: &mut EntryState) -> Option<Waker> {
        if !state.published || !state.captured || state.settled {
            return None;
        }
        state.settled = true;
        state.waker.take()
    }

    pub(crate) fn poll_writable(&self, waker: &Waker) -> Poll<Result<(), SubmitError>> {
        let mut state = self.state.lock().expect("completion entry");
        if state.settled {
            let outcome = match state.outcome.take().expect("live writable outcome") {
                CompletionOutcome::Writable { result, .. } => result,
                CompletionOutcome::Request(_) => unreachable!("request outcome in send entry"),
            };
            state.completion_id = 0;
            state.published = false;
            state.captured = false;
            state.settled = false;
            state.awaiting_writable = false;
            return Poll::Ready(outcome);
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
                CompletionOutcome::Writable { .. } => {
                    unreachable!("writable outcome in request entry")
                }
            };
        }
        if state.waker.as_ref().is_none_or(|old| !old.will_wake(waker)) {
            state.waker = Some(waker.clone());
        }
        Poll::Pending
    }

    /// Blocks the calling thread until the REQUEST settles.
    pub(crate) fn wait_request(&self) -> Result<Vec<Message>, RequestError> {
        let mut state = self.state.lock().expect("completion entry");
        while !state.settled {
            state = self.changed.wait(state).expect("completion entry");
        }
        match state.outcome.take().expect("blocking request outcome") {
            CompletionOutcome::Request(outcome) => outcome,
            CompletionOutcome::Writable { .. } => {
                unreachable!("writable outcome in request entry")
            }
        }
    }

    fn wait_settled(&self) {
        let mut state = self.state.lock().expect("completion entry");
        while !state.settled {
            state = self.changed.wait(state).expect("completion entry");
        }
    }

    /// Detaches only the Rust waiter. Core keeps a live token/request until its
    /// completion or socket lifecycle cleanup. Returns true when the completion
    /// was already captured and the registry entry can be removed now.
    pub(crate) fn detach(&self) -> bool {
        let mut state = self.state.lock().expect("completion entry");
        state.detached = true;
        state.outcome = None;
        state.waker = None;
        state.captured
    }

    fn shutdown(&self) {
        let waker = {
            let mut state = self.state.lock().expect("completion entry");
            if self.kind == CompletionEntryKind::Request && state.settled {
                return;
            }
            state.published = true;
            state.captured = true;
            state.owner_shutdown = true;
            if !state.detached {
                state.outcome = Some(
                    if self.kind == CompletionEntryKind::SendRetry || state.awaiting_writable {
                        CompletionOutcome::Writable {
                            completion_id: state.completion_id,
                            result: Err(submit_error_from_errno(libc::ESHUTDOWN)),
                        }
                    } else {
                        CompletionOutcome::Request(Err(RequestError::new(
                            RequestResult::Terminated,
                            libc::ESHUTDOWN,
                        )))
                    },
                );
            }
            state.settled = false;
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
    parked_writables: HashMap<usize, ParkedWritable>,
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
    drain_gate: Mutex<()>,
    /// Native submits hold this shared; close takes it exclusively so a raw
    /// handle is never entered after the native close returned.
    lifecycle: RwLock<bool>,
    next_context: AtomicUsize,
}

unsafe impl Send for CompletionOwner {}
unsafe impl Sync for CompletionOwner {}

impl CompletionOwner {
    pub(crate) fn new(socket: *mut c_void) -> Arc<Self> {
        Arc::new(Self {
            socket: socket as usize,
            state: Mutex::new(OwnerState {
                entries: HashMap::new(),
                parked_writables: HashMap::new(),
                public_owner: None,
                runtime_poller: 0,
                runtime_thread: None,
                runtime_stop: false,
                shutdown: false,
            }),
            drain_gate: Mutex::new(()),
            lifecycle: RwLock::new(false),
            next_context: AtomicUsize::new(1),
        })
    }

    /// Allocates a never-reused opaque completion context for this socket.
    pub(crate) fn next_context(&self) -> *mut c_void {
        self.next_context.fetch_add(1, Ordering::Relaxed) as *mut c_void
    }

    /// Runs one native submit while the handle is guaranteed open. Concurrent
    /// submits share the guard; close waits for them and then blocks new ones.
    pub(crate) fn with_submit<T>(
        &self,
        action: impl FnOnce() -> Result<T, SubmitError>,
    ) -> Result<T, SubmitError> {
        let closed = self.lifecycle.read().expect("completion lifecycle");
        if *closed {
            return Err(SubmitError::new(SubmitResult::Terminated, libc::ESHUTDOWN));
        }
        action()
    }

    pub(crate) fn register_request(
        self: &Arc<Self>,
    ) -> Result<(Arc<CompletionEntry>, *mut c_void), SubmitError> {
        let entry = CompletionEntry::new(CompletionEntryKind::Request);
        let context = self.next_context();
        let mut state = self.state.lock().expect("completion owner");
        if state.shutdown {
            return Err(SubmitError::new(
                SubmitResult::InvalidState,
                libc::ESHUTDOWN,
            ));
        }
        state.entries.insert(context as usize, Arc::clone(&entry));
        if let Err(error) = self.start_runtime_locked(&mut state) {
            state.entries.remove(&(context as usize));
            return Err(SubmitError::new(
                SubmitResult::InternalError,
                error.native_errno(),
            ));
        }
        Ok((entry, context))
    }

    /// Registers the SEND entry behind `context` for the wait token Core just
    /// issued, replays a WRITABLE record that arrived first, and makes sure a
    /// drain owner is running. The entry stays registered across repeated
    /// backpressure until the waiter unregisters or detaches it.
    pub(crate) fn register_send_token(
        self: &Arc<Self>,
        context: *mut c_void,
        entry: &Arc<CompletionEntry>,
        completion_id: u64,
    ) -> Result<(), SubmitError> {
        let (parked, start_error) = {
            let mut state = self.state.lock().expect("completion owner");
            if state.shutdown {
                return Err(SubmitError::new(SubmitResult::Terminated, libc::ESHUTDOWN));
            }
            state
                .entries
                .entry(context as usize)
                .or_insert_with(|| Arc::clone(entry));
            let parked = state.parked_writables.remove(&(context as usize));
            let start_error = self.start_runtime_locked(&mut state).err();
            (parked, start_error)
        };
        entry.publish_writable(completion_id);
        if let Some(parked) = parked {
            let captured = entry.capture_parked(&parked);
            if captured.detached {
                self.unregister(context);
            }
            if let Some(waker) = captured.waker {
                waker.wake();
            }
        }
        match start_error {
            Some(error) => Err(SubmitError::new(
                SubmitResult::InternalError,
                error.native_errno(),
            )),
            None => Ok(()),
        }
    }

    pub(crate) fn unregister(&self, user_context: *mut c_void) {
        self.state
            .lock()
            .expect("completion owner")
            .entries
            .remove(&(user_context as usize));
    }

    pub(crate) fn drain(self: &Arc<Self>, public_owner: bool) -> Result<usize, RecvError> {
        let drain_guard = self.drain_gate.lock().expect("completion drain gate");
        let mut processed = 0;
        let mut wakers = Vec::new();
        let mut failure = None;
        loop {
            {
                let state = self.state.lock().expect("completion owner");
                if state.shutdown
                    || (public_owner && state.public_owner.is_none())
                    || (!public_owner && (state.runtime_stop || state.public_owner.is_some()))
                {
                    break;
                }
            }
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
                failure = Some(check_recv_rc(rc).expect_err("failed completion receive"));
                break;
            }

            let context = completion.user_context as usize;
            let entry = self
                .state
                .lock()
                .expect("completion owner")
                .entries
                .get(&context)
                .cloned();
            if let Some(entry) = entry {
                let captured = entry.capture(&mut completion);
                if let Some(waker) = captured.waker {
                    wakers.push(waker);
                }
                let request_completion =
                    completion.kind == ffi::zlink_completion_kind_t::ZLINK_COMPLETION_REQUEST;
                if public_owner && entry.kind() == CompletionEntryKind::Request {
                    entry.wait_settled();
                }
                if request_completion || captured.detached {
                    self.unregister(context as *mut c_void);
                }
            } else {
                if context != 0
                    && completion.kind == ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE
                {
                    // The wait token was issued but its SEND registration has
                    // not landed yet; replay this record when it does.
                    let parked = ParkedWritable {
                        completion_id: completion.completion_id,
                        send_result: completion.send_result,
                        send_terminal_errno: completion.send_terminal_errno,
                    };
                    let mut state = self.state.lock().expect("completion owner");
                    if !state.shutdown {
                        state.parked_writables.insert(context, parked);
                    }
                }
                unsafe { ffi::zlink_completion_close(&mut completion) };
            }
            processed += 1;
        }
        drop(drain_guard);
        for waker in wakers {
            waker.wake();
        }
        failure.map_or(Ok(processed), Err)
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
            // Publish ownership before stop_runtime_locked drops the mutex;
            // concurrent registrations must not install a replacement private
            // drainer while the old one is joining.
            state.public_owner = Some(poller_owner);
            state = self.stop_runtime_locked(state);
        }
        drop(state);
        // Wait for a private drain that crossed the ownership check before
        // publication. No new private drain can start while public_owner is set.
        drop(self.drain_gate.lock().expect("completion drain gate"));
        Ok(())
    }

    pub(crate) fn transfer_to_runtime(self: &Arc<Self>, poller_owner: usize) {
        let _drain_guard = self.drain_gate.lock().expect("completion drain gate");
        let mut state = self.state.lock().expect("completion owner");
        if state.public_owner != Some(poller_owner) {
            return;
        }
        state.public_owner = None;
        if !state.entries.is_empty() && !state.shutdown {
            let _ = self.start_runtime_locked(&mut state);
        }
    }

    pub(crate) fn shutdown_with<T>(self: &Arc<Self>, close_native: impl FnOnce() -> T) -> T {
        let mut closed = self.lifecycle.write().expect("completion lifecycle");
        *closed = true;
        self.shutdown_inner();
        close_native()
    }

    fn shutdown_inner(self: &Arc<Self>) {
        {
            let mut state = self.state.lock().expect("completion owner");
            if state.shutdown {
                return;
            }
            state.shutdown = true;
            state = self.stop_runtime_locked(state);
            drop(state);
        }
        let entries = {
            let _drain_guard = self.drain_gate.lock().expect("completion drain gate");
            let mut state = self.state.lock().expect("completion owner");
            state.parked_writables.clear();
            std::mem::take(&mut state.entries)
        };
        for entry in entries.into_values() {
            entry.shutdown();
        }
    }

    fn start_runtime_locked(self: &Arc<Self>, state: &mut OwnerState) -> Result<(), ConfigError> {
        if state.runtime_thread.is_some() || state.shutdown || state.public_owner.is_some() {
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

    /// Retires the calling reactor thread: releases its poller and clears the
    /// thread slot so the next registration can start a fresh reactor.
    fn retire_runtime(&self, owned_poller: usize) {
        let poller = {
            let mut state = self.state.lock().expect("completion owner");
            if state.runtime_stop || state.runtime_poller != owned_poller {
                // A stopper already took the poller and is joining this thread.
                return;
            }
            drop(state.runtime_thread.take());
            std::mem::take(&mut state.runtime_poller)
        };
        if poller != 0 {
            let mut raw = poller as *mut c_void;
            unsafe { ffi::zlink_poller_destroy(&mut raw) };
        }
    }

    fn runtime_loop(self: Arc<Self>) {
        let mut idle_rounds = 0u32;
        let owned_poller = self.state.lock().expect("completion owner").runtime_poller;
        loop {
            {
                let state = self.state.lock().expect("completion owner");
                if state.runtime_stop || state.shutdown || state.runtime_poller != owned_poller {
                    return;
                }
                if state.entries.is_empty() {
                    idle_rounds += 1;
                    if idle_rounds > REACTOR_IDLE_ROUNDS {
                        drop(state);
                        self.retire_runtime(owned_poller);
                        return;
                    }
                } else {
                    idle_rounds = 0;
                }
            }
            let poller = owned_poller as *mut c_void;
            let mut event = ffi::zlink_poller_event_t::empty();
            let rc = unsafe {
                ffi::zlink_poller_wait(poller, &mut event, 1, REACTOR_WAIT_MS, std::ptr::null_mut())
            };
            match rc.cmp(&0) {
                std::cmp::Ordering::Greater => {
                    if self.drain(false).is_err() {
                        self.retire_runtime(owned_poller);
                        return;
                    }
                }
                std::cmp::Ordering::Less => {
                    let errno = unsafe { ffi::zlink_errno() };
                    if errno != libc::EINTR && errno != libc::EAGAIN {
                        self.retire_runtime(owned_poller);
                        return;
                    }
                }
                std::cmp::Ordering::Equal => {}
            }
        }
    }
}

fn writable_outcome(
    kind: ffi::zlink_completion_kind_t,
    completion_id: u64,
    send_result: ffi::zlink_send_complete_result_t,
    send_terminal_errno: i32,
) -> CompletionOutcome {
    let result = if kind != ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE {
        Err(SubmitError::new(SubmitResult::InternalError, libc::EPROTO))
    } else if send_result == ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED
        && send_terminal_errno == 0
    {
        Ok(())
    } else if send_result == ffi::zlink_send_complete_result_t::ZLINK_SEND_TERMINAL
        && send_terminal_errno != 0
    {
        Err(send_terminal_error(send_terminal_errno))
    } else {
        Err(SubmitError::new(SubmitResult::InternalError, libc::EPROTO))
    };
    CompletionOutcome::Writable {
        completion_id,
        result,
    }
}

fn completion_outcome(
    kind: CompletionEntryKind,
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
        CompletionEntryKind::SendRetry => writable_outcome(
            completion.kind,
            completion.completion_id,
            completion.send_result,
            completion.send_terminal_errno,
        ),
        CompletionEntryKind::Request
            if completion.kind == ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE =>
        {
            writable_outcome(
                completion.kind,
                completion.completion_id,
                completion.send_result,
                completion.send_terminal_errno,
            )
        }
        CompletionEntryKind::Request => {
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
                take_completion_parts(completion.reply_parts, completion.reply_part_count)
            };
            CompletionOutcome::Request(outcome)
        }
    }
}

fn take_completion_parts(
    parts: *mut ffi::zlink_msg_t,
    count: usize,
) -> Result<Vec<Message>, RequestError> {
    let mut out = Vec::<Message>::with_capacity(count);
    if parts.is_null() {
        return Ok(out);
    }
    for index in 0..count {
        unsafe {
            // Message owns only its native frame. Adopt into the spare Vec
            // element without constructing or moving an initialized wrapper.
            // Field projection makes no assumption about Rust struct layout.
            let destination = out.as_mut_ptr().add(index);
            let raw = std::ptr::addr_of_mut!((*destination).inner.raw);
            if ffi::zlink_msg_adopt(raw, parts.add(index)) != 0 {
                return Err(RequestError::new(
                    RequestResult::InternalError,
                    ffi::zlink_errno(),
                ));
            }
            // Only successfully initialized elements may be dropped. The
            // completion still owns each emptied source header and closes it.
            out.set_len(index + 1);
        }
    }
    Ok(out)
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
    use crate::message::RoutingId;
    use std::task::Waker;

    #[test]
    fn adopted_reply_parts_outlive_completion_storage_and_shared_sources() {
        for sizes in [vec![], vec![0], vec![64, 0], vec![16, 1024, 65536]] {
            let mut originals: Vec<Message> = sizes
                .iter()
                .enumerate()
                .map(|(index, size)| Message::try_from(vec![index as u8 + 1; *size]).unwrap())
                .collect();
            let mut sources: Vec<ffi::zlink_msg_t> = originals
                .iter_mut()
                .map(|original| unsafe {
                    let mut source = std::mem::MaybeUninit::uninit();
                    assert_eq!(ffi::zlink_msg_init(source.as_mut_ptr()), 0);
                    assert_eq!(
                        ffi::zlink_msg_copy(source.as_mut_ptr(), original.raw_mut()),
                        0
                    );
                    source.assume_init()
                })
                .collect();

            let reply = take_completion_parts(sources.as_mut_ptr(), sources.len()).unwrap();
            for source in &mut sources {
                unsafe {
                    assert_eq!(ffi::zlink_msg_size(source), 0);
                    assert_eq!(ffi::zlink_msg_close(source), 0);
                }
            }
            drop(sources);
            drop(originals);

            assert_eq!(reply.len(), sizes.len());
            for (index, (part, size)) in reply.iter().zip(&sizes).enumerate() {
                assert_eq!(part.as_bytes(), vec![index as u8 + 1; *size]);
                assert_eq!(part.ref_count(), 1);
            }
        }
    }

    #[test]
    fn failed_reply_adoption_releases_only_the_adopted_prefix() {
        let mut original = Message::try_from(vec![7; 1024]).unwrap();
        let mut sources: [ffi::zlink_msg_t; 3] = std::array::from_fn(|_| unsafe {
            let mut source = std::mem::MaybeUninit::uninit();
            assert_eq!(ffi::zlink_msg_init(source.as_mut_ptr()), 0);
            assert_eq!(
                ffi::zlink_msg_copy(source.as_mut_ptr(), original.raw_mut()),
                0
            );
            source.assume_init()
        });
        unsafe {
            assert_eq!(ffi::zlink_msg_close(&mut sources[1]), 0);
        }

        let error = take_completion_parts(sources.as_mut_ptr(), sources.len())
            .err()
            .expect("invalid native source");
        assert_eq!(error.code(), RequestResult::InternalError);
        assert_eq!(error.native_errno(), libc::EFAULT);
        // The first source was adopted then dropped on error; the last source
        // remains owned by the completion, together with its empty prefix.
        assert_eq!(original.ref_count(), 2);
        unsafe {
            assert_eq!(ffi::zlink_msg_size(&sources[0]), 0);
            assert_eq!(ffi::zlink_msg_size(&sources[2]), 1024);
            assert_eq!(ffi::zlink_msg_close(&mut sources[0]), 0);
            assert_eq!(ffi::zlink_msg_close(&mut sources[2]), 0);
        }
        assert_eq!(original.ref_count(), 1);
    }

    #[test]
    fn capture_before_publish_joins_exactly_once() {
        let entry = CompletionEntry::new(CompletionEntryKind::SendRetry);
        let mut completion = ffi::zlink_completion_t::empty();
        completion.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE;
        completion.completion_id = 41;
        completion.send_result = ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED;
        entry.capture(&mut completion);

        let waker = Waker::noop();
        assert!(entry.poll_writable(waker).is_pending());
        entry.publish_writable(41);
        assert!(matches!(entry.poll_writable(waker), Poll::Ready(Ok(()))));
    }

    #[test]
    fn parked_writable_replays_like_a_live_record() {
        let entry = CompletionEntry::new(CompletionEntryKind::SendRetry);
        entry.publish_writable(71);
        let parked = ParkedWritable {
            completion_id: 71,
            send_result: ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED,
            send_terminal_errno: 0,
        };
        let captured = entry.capture_parked(&parked);
        assert!(!captured.detached);
        let waker = Waker::noop();
        assert!(matches!(entry.poll_writable(waker), Poll::Ready(Ok(()))));
    }

    #[test]
    fn mismatched_writable_token_cannot_resume_the_expected_waiter() {
        let entry = CompletionEntry::new(CompletionEntryKind::SendRetry);
        entry.publish_writable(41);
        let mut wrong = ffi::zlink_completion_t::empty();
        wrong.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE;
        wrong.completion_id = 42;
        wrong.send_result = ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED;
        entry.capture(&mut wrong);

        let waker = Waker::noop();
        assert!(entry.poll_writable(waker).is_pending());

        let mut expected = ffi::zlink_completion_t::empty();
        expected.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE;
        expected.completion_id = 41;
        expected.send_result = ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED;
        entry.capture(&mut expected);
        assert!(matches!(entry.poll_writable(waker), Poll::Ready(Ok(()))));
    }

    #[test]
    fn writable_for_waiter_token_is_delivered_without_peer_rid_reverification() {
        for kind in [CompletionEntryKind::SendRetry, CompletionEntryKind::Request] {
            for capture_first in [false, true] {
                let entry = CompletionEntry::new(kind);
                let waker = Waker::noop();
                let mut completion = ffi::zlink_completion_t::empty();
                completion.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE;
                completion.completion_id = 51;
                // Deliberately vary Core-owned metadata: only the wait token
                // determines which packet resumes in the binding.
                completion.peer_rid = *RoutingId::from(b"core-peer").as_raw();
                completion.send_result = ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED;

                if capture_first {
                    entry.capture(&mut completion);
                    assert!(entry.poll_writable(waker).is_pending());
                    entry.publish_writable(51);
                } else {
                    entry.publish_writable(51);
                    assert!(entry.poll_writable(waker).is_pending());
                    entry.capture(&mut completion);
                }
                assert!(matches!(entry.poll_writable(waker), Poll::Ready(Ok(()))));
                assert!(entry.poll_writable(waker).is_pending());
            }
        }
    }

    #[test]
    fn writable_entry_rejects_non_writable_completion_kind() {
        let entry = CompletionEntry::new(CompletionEntryKind::SendRetry);
        let waker = Waker::noop();

        entry.publish_writable(51);
        let mut wrong_kind = ffi::zlink_completion_t::empty();
        wrong_kind.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_REQUEST;
        wrong_kind.completion_id = 51;
        entry.capture(&mut wrong_kind);
        assert!(matches!(
            entry.poll_writable(waker),
            Poll::Ready(Err(error))
                if error.code() == SubmitResult::InternalError
                    && error.native_errno() == libc::EPROTO
        ));
    }

    #[test]
    fn writable_entry_rearms_with_each_new_token() {
        let target = RoutingId::from(b"expected-target");
        let entry = CompletionEntry::new(CompletionEntryKind::SendRetry);
        let waker = Waker::noop();

        for token in [52, 53] {
            entry.publish_writable(token);
            let mut completion = ffi::zlink_completion_t::empty();
            completion.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE;
            completion.completion_id = token;
            completion.peer_rid = *target.as_raw();
            completion.send_result = ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED;
            entry.capture(&mut completion);
            assert!(matches!(entry.poll_writable(waker), Poll::Ready(Ok(()))));
        }
    }

    #[test]
    fn request_entry_moves_from_writable_to_request_completion() {
        let entry = CompletionEntry::new(CompletionEntryKind::Request);
        let waker = Waker::noop();

        entry.publish_writable(54);
        let mut writable = ffi::zlink_completion_t::empty();
        writable.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE;
        writable.completion_id = 54;
        writable.send_result = ffi::zlink_send_complete_result_t::ZLINK_SEND_ADMITTED;
        entry.capture(&mut writable);
        assert!(matches!(entry.poll_writable(waker), Poll::Ready(Ok(()))));

        entry.publish_request(55);
        let mut request = ffi::zlink_completion_t::empty();
        request.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_REQUEST;
        request.completion_id = 55;
        request.request_result = ffi::zlink_request_result_t::ZLINK_REQUEST_OK;
        entry.capture(&mut request);
        assert!(matches!(
            entry.poll_request(waker),
            Poll::Ready(Ok(parts)) if parts.is_empty()
        ));
    }

    #[test]
    fn request_writable_shutdown_is_a_typed_submit_failure() {
        let entry = CompletionEntry::new(CompletionEntryKind::Request);
        entry.publish_writable(56);
        entry.shutdown();
        let waker = Waker::noop();
        assert!(matches!(
            entry.poll_writable(waker),
            Poll::Ready(Err(error))
                if error.code() == SubmitResult::Terminated
                    && error.native_errno() == libc::ESHUTDOWN
        ));
    }

    #[test]
    fn terminal_writable_maps_to_a_terminal_submit_error() {
        for (errno, expected) in [
            (libc::ESHUTDOWN, SubmitResult::Terminated),
            (libc::ENOENT, SubmitResult::NotFound),
            (156_384_765, SubmitResult::Terminated),
        ] {
            let entry = CompletionEntry::new(CompletionEntryKind::SendRetry);
            entry.publish_writable(61);
            let mut completion = ffi::zlink_completion_t::empty();
            completion.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_WRITABLE;
            completion.completion_id = 61;
            completion.send_result = ffi::zlink_send_complete_result_t::ZLINK_SEND_TERMINAL;
            completion.send_terminal_errno = errno;
            entry.capture(&mut completion);

            let waker = Waker::noop();
            assert!(matches!(
                entry.poll_writable(waker),
                Poll::Ready(Err(error))
                    if error.code() == expected && error.native_errno() == errno
            ));
        }
    }

    #[test]
    fn detached_late_completion_discards_outcome_after_cleanup() {
        let entry = CompletionEntry::new(CompletionEntryKind::Request);
        entry.detach();
        let mut completion = ffi::zlink_completion_t::empty();
        completion.kind = ffi::zlink_completion_kind_t::ZLINK_COMPLETION_REQUEST;
        completion.completion_id = 52;
        completion.request_result = ffi::zlink_request_result_t::ZLINK_REQUEST_TIMED_OUT;
        entry.capture(&mut completion);
        entry.publish_request(52);

        let state = entry.state.lock().expect("completion state");
        assert!(state.settled);
        assert!(state.outcome.is_none());
    }
}
