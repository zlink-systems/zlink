use std::cmp::Ordering as CmpOrdering;
use std::collections::{BinaryHeap, HashMap};
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex, OnceLock, Weak};
use std::task::Waker;
use std::thread;
use std::time::{Duration, Instant};

use crate::ffi;
use crate::message::RoutingId;
use crate::native_errors::submit_error_from_errno;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RoutedRole {
    Dealer,
    Router,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct RoutedTargetKey {
    peer_rid: ffi::zlink_routing_id_t,
    transport_pair_id: u64,
    transport_pair_generation: u64,
}

impl RoutedTargetKey {
    pub(crate) fn from_target(target: &ffi::zlink_routed_submit_target_t) -> Self {
        Self {
            peer_rid: target.peer_rid,
            transport_pair_id: target.transport_pair_id,
            transport_pair_generation: target.transport_pair_generation,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RoutedWakeOutcome {
    Waiting,
    Ready,
    Terminal(i32),
    Deadline,
}

struct RoutedWakeState {
    outcome: RoutedWakeOutcome,
    waker: Option<Waker>,
}

pub(crate) struct RoutedWake {
    state: Mutex<RoutedWakeState>,
}

impl RoutedWake {
    pub(crate) fn new() -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(RoutedWakeState {
                outcome: RoutedWakeOutcome::Waiting,
                waker: None,
            }),
        })
    }

    pub(crate) fn register_waker(&self, waker: &Waker) -> RoutedWakeOutcome {
        let mut state = self.state.lock().expect("routed wake state");
        if state
            .waker
            .as_ref()
            .is_none_or(|registered| !registered.will_wake(waker))
        {
            state.waker = Some(waker.clone());
        }
        state.outcome
    }

    pub(crate) fn take_ready(&self) -> RoutedWakeOutcome {
        let mut state = self.state.lock().expect("routed wake state");
        match state.outcome {
            RoutedWakeOutcome::Ready => {
                state.outcome = RoutedWakeOutcome::Waiting;
                RoutedWakeOutcome::Ready
            }
            outcome => outcome,
        }
    }

    pub(crate) fn notify_ready(&self) {
        self.set_outcome(RoutedWakeOutcome::Ready, false);
    }

    pub(crate) fn notify_terminal(&self, errno: i32) {
        self.set_outcome(RoutedWakeOutcome::Terminal(errno), true);
    }

    pub(crate) fn notify_deadline(&self) {
        self.set_outcome(RoutedWakeOutcome::Deadline, true);
    }

    fn set_outcome(&self, outcome: RoutedWakeOutcome, terminal: bool) {
        let waker = {
            let mut state = self.state.lock().expect("routed wake state");
            if matches!(
                state.outcome,
                RoutedWakeOutcome::Terminal(_) | RoutedWakeOutcome::Deadline
            ) {
                return;
            }
            if !terminal && state.outcome == RoutedWakeOutcome::Ready {
                return;
            }
            state.outcome = outcome;
            state.waker.clone()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

pub(crate) struct RoutedAdmission {
    handle: AtomicPtr<c_void>,
    role: RoutedRole,
    attempt_gate: Mutex<()>,
    pending: Mutex<HashMap<RoutedTargetKey, Vec<Weak<RoutedWake>>>>,
}

unsafe impl Send for RoutedAdmission {}
unsafe impl Sync for RoutedAdmission {}

impl RoutedAdmission {
    pub(crate) fn new(handle: *mut c_void, role: RoutedRole) -> Arc<Self> {
        Arc::new(Self {
            handle: AtomicPtr::new(handle),
            role,
            attempt_gate: Mutex::new(()),
            pending: Mutex::new(HashMap::new()),
        })
    }

    pub(crate) fn role(&self) -> RoutedRole {
        self.role
    }

    pub(crate) fn handle(&self) -> *mut c_void {
        self.handle.load(Ordering::Acquire)
    }

    pub(crate) fn with_attempt_gate<T>(&self, attempt: impl FnOnce(*mut c_void) -> T) -> Option<T> {
        let _gate = self.attempt_gate.lock().expect("routed attempt gate");
        let handle = self.handle();
        (!handle.is_null()).then(|| attempt(handle))
    }

    pub(crate) fn select_target(
        &self,
        router_rid: Option<&RoutingId>,
    ) -> (i32, ffi::zlink_routed_submit_target_t) {
        let _gate = self.attempt_gate.lock().expect("routed attempt gate");
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

    pub(crate) fn send_timeout(&self) -> Result<Duration, crate::error::SubmitError> {
        self.get_common_timeout(ffi::zlink_option_t::ZLINK_OPT_SNDTIMEO)
    }

    pub(crate) fn request_timeout(&self) -> Result<Duration, crate::error::SubmitError> {
        let _gate = self.attempt_gate.lock().expect("routed attempt gate");
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

    fn get_common_timeout(
        &self,
        option: ffi::zlink_option_t,
    ) -> Result<Duration, crate::error::SubmitError> {
        let _gate = self.attempt_gate.lock().expect("routed attempt gate");
        let handle = self.handle();
        if handle.is_null() {
            return Err(submit_error_from_errno(libc::ECANCELED));
        }
        let mut millis = 0i32;
        let mut len = std::mem::size_of::<i32>();
        let rc = unsafe {
            ffi::zlink_get_option(handle, option, (&mut millis as *mut i32).cast(), &mut len)
        };
        if rc != 0 {
            return Err(submit_error_from_errno(unsafe { ffi::zlink_errno() }));
        }
        Ok(Duration::from_millis(millis.max(0) as u64))
    }

    pub(crate) fn register(&self, key: RoutedTargetKey, wake: &Arc<RoutedWake>) {
        let mut pending = self.pending.lock().expect("routed pending state");
        let waiters = pending.entry(key).or_default();
        waiters.retain(|candidate| candidate.strong_count() != 0);
        if !waiters
            .iter()
            .filter_map(Weak::upgrade)
            .any(|candidate| Arc::ptr_eq(&candidate, wake))
        {
            waiters.push(Arc::downgrade(wake));
        }
    }

    pub(crate) fn unregister(&self, key: RoutedTargetKey, wake: &Arc<RoutedWake>) {
        let mut pending = self.pending.lock().expect("routed pending state");
        if let Some(waiters) = pending.get_mut(&key) {
            waiters.retain(|candidate| {
                candidate
                    .upgrade()
                    .is_some_and(|candidate| !Arc::ptr_eq(&candidate, wake))
            });
            if waiters.is_empty() {
                pending.remove(&key);
            }
        }
    }

    pub(crate) fn on_ready_event(&self, event: &ffi::zlink_routed_send_ready_event_t) {
        let key = RoutedTargetKey {
            peer_rid: event.peer_rid,
            transport_pair_id: event.transport_pair_id,
            transport_pair_generation: event.transport_pair_generation,
        };
        let waiters = {
            let mut pending = self.pending.lock().expect("routed pending state");
            let Some(waiters) = pending.get_mut(&key) else {
                return;
            };
            let live: Vec<_> = waiters.iter().filter_map(Weak::upgrade).collect();
            waiters.retain(|candidate| candidate.strong_count() != 0);
            if event.state == ffi::zlink_routed_send_ready_state_t::ZLINK_ROUTED_SEND_TERMINAL {
                pending.remove(&key);
            }
            live
        };
        for waiter in waiters {
            match event.state {
                ffi::zlink_routed_send_ready_state_t::ZLINK_ROUTED_SEND_WRITABLE => {
                    waiter.notify_ready()
                }
                ffi::zlink_routed_send_ready_state_t::ZLINK_ROUTED_SEND_TERMINAL => {
                    waiter.notify_terminal(event.terminal_errno)
                }
            }
        }
    }

    pub(crate) fn close_native(&self, detach_on_failure: bool) -> i32 {
        let _gate = self.attempt_gate.lock().expect("routed attempt gate");
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

    pub(crate) fn notify_closed(&self, errno: i32) {
        self.handle.store(std::ptr::null_mut(), Ordering::Release);
        let waiters = {
            let mut pending = self.pending.lock().expect("routed pending state");
            let waiters = pending
                .values()
                .flat_map(|values| values.iter().filter_map(Weak::upgrade))
                .collect::<Vec<_>>();
            pending.clear();
            waiters
        };
        for waiter in waiters {
            waiter.notify_terminal(errno);
        }
    }
}

pub(crate) unsafe extern "C" fn routed_ready_trampoline(
    _subject: *mut c_void,
    event: *const ffi::zlink_routed_send_ready_event_t,
    userdata: *mut c_void,
) {
    if event.is_null() {
        return;
    }
    let _ = unsafe {
        super::CallbackBox::invoke::<Arc<RoutedAdmission>, _>(userdata, |admission| {
            admission.on_ready_event(&*event)
        })
    };
}

pub(crate) struct DeadlineToken {
    active: AtomicBool,
    wake: Weak<RoutedWake>,
}

impl DeadlineToken {
    pub(crate) fn schedule(deadline: Instant, wake: &Arc<RoutedWake>) -> Arc<Self> {
        let token = Arc::new(Self {
            active: AtomicBool::new(true),
            wake: Arc::downgrade(wake),
        });
        deadline_reactor()
            .send(DeadlineEntry::new(deadline, &token))
            .expect("routed deadline reactor stopped");
        token
    }

    pub(crate) fn cancel(&self) {
        self.active.store(false, Ordering::Release);
    }

    fn fire(&self) {
        if self.active.swap(false, Ordering::AcqRel) {
            if let Some(wake) = self.wake.upgrade() {
                wake.notify_deadline();
            }
        }
    }
}

struct DeadlineEntry {
    deadline: Instant,
    sequence: u64,
    token: Weak<DeadlineToken>,
}

impl DeadlineEntry {
    fn new(deadline: Instant, token: &Arc<DeadlineToken>) -> Self {
        static NEXT_SEQUENCE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
        Self {
            deadline,
            sequence: NEXT_SEQUENCE.fetch_add(1, Ordering::Relaxed),
            token: Arc::downgrade(token),
        }
    }
}

impl PartialEq for DeadlineEntry {
    fn eq(&self, other: &Self) -> bool {
        self.deadline == other.deadline && self.sequence == other.sequence
    }
}

impl Eq for DeadlineEntry {}

impl PartialOrd for DeadlineEntry {
    fn partial_cmp(&self, other: &Self) -> Option<CmpOrdering> {
        Some(self.cmp(other))
    }
}

impl Ord for DeadlineEntry {
    fn cmp(&self, other: &Self) -> CmpOrdering {
        other
            .deadline
            .cmp(&self.deadline)
            .then_with(|| other.sequence.cmp(&self.sequence))
    }
}

fn deadline_reactor() -> &'static Sender<DeadlineEntry> {
    static REACTOR: OnceLock<Sender<DeadlineEntry>> = OnceLock::new();
    REACTOR.get_or_init(|| {
        let (sender, receiver) = mpsc::channel();
        thread::Builder::new()
            .name("zlink-rust-deadline".to_owned())
            .spawn(move || run_deadline_reactor(receiver))
            .expect("start routed deadline reactor");
        sender
    })
}

fn run_deadline_reactor(receiver: Receiver<DeadlineEntry>) {
    let mut deadlines = BinaryHeap::<DeadlineEntry>::new();
    loop {
        while deadlines
            .peek()
            .is_some_and(|entry| entry.deadline <= Instant::now())
        {
            if let Some(entry) = deadlines.pop() {
                if let Some(token) = entry.token.upgrade() {
                    token.fire();
                }
            }
        }

        let received = match deadlines.peek() {
            Some(entry) => {
                receiver.recv_timeout(entry.deadline.saturating_duration_since(Instant::now()))
            }
            None => receiver.recv().map_err(|_| RecvTimeoutError::Disconnected),
        };
        match received {
            Ok(entry) => deadlines.push(entry),
            Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => return,
        }
    }
}

pub(crate) fn duration_until(deadline: Instant) -> Duration {
    deadline.saturating_duration_since(Instant::now())
}
