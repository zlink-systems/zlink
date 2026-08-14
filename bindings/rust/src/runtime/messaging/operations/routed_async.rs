// SPDX-License-Identifier: MPL-2.0

use std::ffi::c_void;
use std::future::Future;
use std::marker::PhantomData;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Waker};
use std::time::{Duration, Instant};

use crate::error::{RequestError, RequestResult, SubmitError, SubmitResult, ZlinkError};
use crate::ffi;
use crate::internal::{
    DeadlineToken, RoutedAdmission, RoutedRole, RoutedTargetKey, RoutedWake, RoutedWakeOutcome,
    duration_until,
};
use crate::message::{Message, RoutingId};
use crate::messaging_operations::{
    Empty, MessageParts, RequestOp, RequestOpStorage, RoutedSendOpStorage,
};
use crate::native_errors::{request_error_from_result, submit_error_from_errno};

pub(crate) fn submit_routed_send(
    operation: RoutedSendOpStorage,
) -> impl Future<Output = Result<(), SubmitError>> + Send {
    RoutedSendFuture::new(operation)
}

pub(crate) fn dealer_request_op(admission: Arc<RoutedAdmission>) -> RequestOp<Empty> {
    RequestOp {
        inner: RequestOpStorage {
            admission,
            peer_rid: None,
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
        },
        _state: PhantomData,
    }
}

pub(crate) fn router_request_op(
    admission: Arc<RoutedAdmission>,
    peer_rid: RoutingId,
) -> RequestOp<Empty> {
    RequestOp {
        inner: RequestOpStorage {
            admission,
            peer_rid: Some(peer_rid),
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
        },
        _state: PhantomData,
    }
}

pub(crate) fn submit_routed_request(
    operation: RequestOpStorage,
) -> impl Future<Output = Result<Vec<Message>, ZlinkError>> + Send {
    RoutedRequestFuture::new(operation)
}

struct PendingTarget {
    target: ffi::zlink_routed_submit_target_t,
    key: RoutedTargetKey,
}

struct RoutedSendFuture {
    operation: Option<RoutedSendOpStorage>,
    pending: Option<PendingTarget>,
    wake: Arc<RoutedWake>,
    deadline: Option<Instant>,
    deadline_token: Option<Arc<DeadlineToken>>,
    timeout_error: Option<SubmitError>,
    waiting: bool,
    attempted_once: bool,
    finished: bool,
}

impl RoutedSendFuture {
    fn new(operation: RoutedSendOpStorage) -> Self {
        let started = Instant::now();
        let (deadline, timeout_error) = match operation.admission.send_timeout() {
            Ok(timeout) => (Some(started + timeout), None),
            Err(error) => (None, Some(error)),
        };
        Self {
            operation: Some(operation),
            pending: None,
            wake: RoutedWake::new(),
            deadline,
            deadline_token: None,
            timeout_error,
            waiting: false,
            attempted_once: false,
            finished: false,
        }
    }

    fn unregister(&mut self) {
        if let (Some(operation), Some(pending)) = (&self.operation, &self.pending) {
            operation.admission.unregister(pending.key, &self.wake);
        }
        if let Some(token) = self.deadline_token.take() {
            token.cancel();
        }
        self.pending = None;
    }

    fn finish(&mut self) {
        self.unregister();
        self.operation.take();
        self.finished = true;
    }

    fn ensure_target(&mut self) -> Result<(), SubmitError> {
        if self.pending.is_some() {
            return Ok(());
        }
        let operation = self.operation.as_ref().expect("active routed send");
        let (rc, target) = operation.admission.select_target(operation.target.as_ref());
        if rc != 0 {
            return Err(submit_error_from_errno(unsafe { ffi::zlink_errno() }));
        }
        let key = RoutedTargetKey::from_target(&target);
        operation.admission.register(key, &self.wake);
        self.pending = Some(PendingTarget { target, key });
        Ok(())
    }

    fn attempt(&mut self) -> Result<i32, SubmitError> {
        let operation = self.operation.as_ref().expect("active routed send");
        let mut parts = clone_parts(&operation.parts)?;
        let target = self.pending.as_ref().expect("selected target").target;
        let role = operation.admission.role();
        operation
            .admission
            .with_attempt_gate(|handle| {
                submit_exact_send(handle, role, operation.target.as_ref(), &target, &mut parts)
            })
            .ok_or_else(|| submit_error_from_errno(libc::ECANCELED))
    }
}

impl Future for RoutedSendFuture {
    type Output = Result<(), SubmitError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.finished {
            panic!("routed send Future polled after completion");
        }
        if let Some(error) = self.timeout_error.take() {
            self.finish();
            return Poll::Ready(Err(error));
        }

        let outcome = self.wake.register_waker(cx.waker());
        match outcome {
            RoutedWakeOutcome::Terminal(errno) => {
                self.finish();
                return Poll::Ready(Err(submit_error_from_errno(errno)));
            }
            RoutedWakeOutcome::Deadline if self.attempted_once => {
                self.finish();
                return Poll::Ready(Err(timeout_submit_error()));
            }
            _ => {}
        }

        if self.waiting {
            match self.wake.take_ready() {
                RoutedWakeOutcome::Ready => self.waiting = false,
                RoutedWakeOutcome::Terminal(errno) => {
                    self.finish();
                    return Poll::Ready(Err(submit_error_from_errno(errno)));
                }
                RoutedWakeOutcome::Deadline => {
                    self.finish();
                    return Poll::Ready(Err(timeout_submit_error()));
                }
                RoutedWakeOutcome::Waiting => return Poll::Pending,
            }
        }

        if let Err(error) = self.ensure_target() {
            self.finish();
            return Poll::Ready(Err(error));
        }

        self.attempted_once = true;
        let rc = match self.attempt() {
            Ok(rc) => rc,
            Err(error) => {
                self.finish();
                return Poll::Ready(Err(error));
            }
        };
        if rc == SubmitResult::Ok as i32 {
            self.finish();
            return Poll::Ready(Ok(()));
        }
        if rc != SubmitResult::Backpressured as i32 {
            let error = submit_error_from_errno(unsafe { ffi::zlink_errno() });
            self.finish();
            return Poll::Ready(Err(error));
        }

        if self
            .deadline
            .is_some_and(|deadline| deadline <= Instant::now())
        {
            self.finish();
            return Poll::Ready(Err(timeout_submit_error()));
        }
        if self.deadline_token.is_none() {
            self.deadline_token = self
                .deadline
                .map(|deadline| DeadlineToken::schedule(deadline, &self.wake));
        }
        self.waiting = true;
        Poll::Pending
    }
}

impl Drop for RoutedSendFuture {
    fn drop(&mut self) {
        if !self.finished {
            self.unregister();
        }
    }
}

struct RequestCompletionState {
    outcome: Option<Result<Vec<Message>, RequestError>>,
    waker: Option<Waker>,
    detached: bool,
}

struct RequestCompletion {
    state: Mutex<RequestCompletionState>,
}

impl RequestCompletion {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(RequestCompletionState {
                outcome: None,
                waker: None,
                detached: false,
            }),
        })
    }

    fn poll(&self, waker: &Waker) -> Poll<Result<Vec<Message>, RequestError>> {
        let mut state = self.state.lock().expect("request completion state");
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

    fn complete(&self, outcome: Result<Vec<Message>, RequestError>) {
        let waker = {
            let mut state = self.state.lock().expect("request completion state");
            if state.detached || state.outcome.is_some() {
                return;
            }
            state.outcome = Some(outcome);
            state.waker.clone()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }

    fn detach(&self) {
        let mut state = self.state.lock().expect("request completion state");
        state.detached = true;
        state.outcome.take();
        state.waker = None;
    }
}

struct RoutedRequestFuture {
    operation: Option<RequestOpStorage>,
    pending: Option<PendingTarget>,
    wake: Arc<RoutedWake>,
    completion: Arc<RequestCompletion>,
    deadline: Option<Instant>,
    deadline_token: Option<Arc<DeadlineToken>>,
    init_error: Option<SubmitError>,
    waiting: bool,
    attempted_once: bool,
    accepted: bool,
    finished: bool,
}

impl RoutedRequestFuture {
    fn new(operation: RequestOpStorage) -> Self {
        let started = Instant::now();
        let timeout = if operation.timeout.is_zero() {
            operation.admission.request_timeout()
        } else {
            Ok(operation.timeout)
        };
        let (deadline, init_error) = match timeout {
            Ok(timeout) => (Some(started + timeout), None),
            Err(error) => (None, Some(error)),
        };
        Self {
            operation: Some(operation),
            pending: None,
            wake: RoutedWake::new(),
            completion: RequestCompletion::new(),
            deadline,
            deadline_token: None,
            init_error,
            waiting: false,
            attempted_once: false,
            accepted: false,
            finished: false,
        }
    }

    fn unregister(&mut self) {
        if let (Some(operation), Some(pending)) = (&self.operation, &self.pending) {
            operation.admission.unregister(pending.key, &self.wake);
        }
        if let Some(token) = self.deadline_token.take() {
            token.cancel();
        }
        self.pending = None;
    }

    fn finish(&mut self) {
        self.unregister();
        self.operation.take();
        self.finished = true;
    }

    fn ensure_target(&mut self) -> Result<(), SubmitError> {
        if self.pending.is_some() {
            return Ok(());
        }
        let operation = self.operation.as_ref().expect("active routed request");
        let (rc, target) = operation
            .admission
            .select_target(operation.peer_rid.as_ref());
        if rc != 0 {
            return Err(submit_error_from_errno(unsafe { ffi::zlink_errno() }));
        }
        let key = RoutedTargetKey::from_target(&target);
        operation.admission.register(key, &self.wake);
        self.pending = Some(PendingTarget { target, key });
        Ok(())
    }

    fn attempt(&mut self) -> Result<i32, SubmitError> {
        let operation = self.operation.as_ref().expect("active routed request");
        let mut parts = clone_parts(&operation.parts)?;
        let target = self.pending.as_ref().expect("selected target").target;
        let role = operation.admission.role();
        let remaining = self
            .deadline
            .map(duration_until)
            .map(duration_to_timeout_ms)
            .unwrap_or(0);
        if remaining == 0 && self.attempted_once {
            return Ok(SubmitResult::Backpressured as i32);
        }

        let callback_owner = Arc::into_raw(Arc::clone(&self.completion));
        let result = operation.admission.with_attempt_gate(|handle| {
            submit_exact_request(
                handle,
                role,
                operation.peer_rid.as_ref(),
                &target,
                &mut parts,
                remaining,
                callback_owner.cast_mut().cast(),
            )
        });
        let rc = match result {
            Some(rc) => rc,
            None => {
                unsafe {
                    drop(Arc::from_raw(callback_owner));
                }
                return Err(submit_error_from_errno(libc::ECANCELED));
            }
        };
        if rc != SubmitResult::Ok as i32 {
            unsafe {
                drop(Arc::from_raw(callback_owner));
            }
        }
        Ok(rc)
    }
}

impl Future for RoutedRequestFuture {
    type Output = Result<Vec<Message>, ZlinkError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.finished {
            panic!("routed request Future polled after completion");
        }
        if self.accepted {
            return match self.completion.poll(cx.waker()) {
                Poll::Ready(Ok(parts)) => {
                    self.finished = true;
                    Poll::Ready(Ok(parts))
                }
                Poll::Ready(Err(error)) => {
                    self.finished = true;
                    Poll::Ready(Err(error.into()))
                }
                Poll::Pending => Poll::Pending,
            };
        }
        if let Some(error) = self.init_error.take() {
            self.finish();
            return Poll::Ready(Err(error.into()));
        }
        if !self.attempted_once
            && self
                .deadline
                .is_some_and(|deadline| deadline <= Instant::now())
        {
            self.finish();
            return Poll::Ready(Err(timeout_request_error().into()));
        }

        let outcome = self.wake.register_waker(cx.waker());
        match outcome {
            RoutedWakeOutcome::Terminal(errno) => {
                self.finish();
                return Poll::Ready(Err(submit_error_from_errno(errno).into()));
            }
            RoutedWakeOutcome::Deadline if self.attempted_once => {
                self.finish();
                return Poll::Ready(Err(timeout_request_error().into()));
            }
            _ => {}
        }
        if self.waiting {
            match self.wake.take_ready() {
                RoutedWakeOutcome::Ready => self.waiting = false,
                RoutedWakeOutcome::Terminal(errno) => {
                    self.finish();
                    return Poll::Ready(Err(submit_error_from_errno(errno).into()));
                }
                RoutedWakeOutcome::Deadline => {
                    self.finish();
                    return Poll::Ready(Err(timeout_request_error().into()));
                }
                RoutedWakeOutcome::Waiting => return Poll::Pending,
            }
        }

        if let Err(error) = self.ensure_target() {
            self.finish();
            return Poll::Ready(Err(error.into()));
        }

        let rc = match self.attempt() {
            Ok(rc) => rc,
            Err(error) => {
                self.finish();
                return Poll::Ready(Err(error.into()));
            }
        };
        self.attempted_once = true;
        if rc == SubmitResult::Ok as i32 {
            self.unregister();
            self.operation.take();
            self.accepted = true;
            return match self.completion.poll(cx.waker()) {
                Poll::Ready(Ok(parts)) => {
                    self.finished = true;
                    Poll::Ready(Ok(parts))
                }
                Poll::Ready(Err(error)) => {
                    self.finished = true;
                    Poll::Ready(Err(error.into()))
                }
                Poll::Pending => Poll::Pending,
            };
        }
        if rc != SubmitResult::Backpressured as i32 {
            let error = submit_error_from_errno(unsafe { ffi::zlink_errno() });
            self.finish();
            return Poll::Ready(Err(error.into()));
        }

        if self
            .deadline
            .is_some_and(|deadline| deadline <= Instant::now())
        {
            self.finish();
            return Poll::Ready(Err(timeout_request_error().into()));
        }
        if self.deadline_token.is_none() {
            self.deadline_token = self
                .deadline
                .map(|deadline| DeadlineToken::schedule(deadline, &self.wake));
        }
        self.waiting = true;
        Poll::Pending
    }
}

impl Drop for RoutedRequestFuture {
    fn drop(&mut self) {
        if self.accepted {
            self.completion.detach();
        } else if !self.finished {
            self.unregister();
        }
    }
}

unsafe extern "C" fn routed_reply_callback(
    result: ffi::zlink_request_result_t,
    parts: *mut ffi::zlink_msg_t,
    part_count: usize,
    userdata: *mut c_void,
) {
    if userdata.is_null() {
        if !parts.is_null() {
            unsafe { ffi::zlink_multipart_close(parts, part_count) };
        }
        return;
    }
    let completion = unsafe { Arc::from_raw(userdata.cast::<RequestCompletion>()) };
    let received = crate::socket::take_parts(parts, part_count);
    let outcome = if result == ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_OK {
        Ok(received)
    } else {
        drop(received);
        Err(request_error_from_result(request_result_from_native(
            result,
        )))
    };
    completion.complete(outcome);
}

fn clone_parts(parts: &MessageParts) -> Result<Vec<Message>, SubmitError> {
    parts
        .iter()
        .map(|part| {
            part.try_clone().map_err(|error| {
                submit_error_from_errno(if error.native_errno() == 0 {
                    libc::ENOMEM
                } else {
                    error.native_errno()
                })
            })
        })
        .collect()
}

fn submit_exact_send(
    handle: *mut c_void,
    role: RoutedRole,
    router_rid: Option<&RoutingId>,
    target: &ffi::zlink_routed_submit_target_t,
    parts: &mut [Message],
) -> i32 {
    let count = parts.len();
    for (index, part) in parts.iter_mut().enumerate() {
        let flag = if index + 1 == count {
            ffi::zlink_part_flag_t::ZLINK_PART_FINAL
        } else {
            ffi::zlink_part_flag_t::ZLINK_PART_MORE
        };
        let rc = unsafe {
            match role {
                RoutedRole::Dealer => ffi::zlink_dealer_send_transport_pair_part(
                    handle,
                    target,
                    part.raw_mut(),
                    ffi::ZLINK_DONTWAIT,
                    flag,
                ),
                RoutedRole::Router => ffi::zlink_send_part_transport_pair(
                    handle,
                    router_rid.expect("router target").as_raw(),
                    target.transport_pair_id,
                    target.transport_pair_generation,
                    part.raw_mut(),
                    ffi::ZLINK_DONTWAIT,
                    flag,
                ),
            }
        };
        if rc != SubmitResult::Ok as i32 {
            return rc;
        }
    }
    SubmitResult::Ok as i32
}

fn submit_exact_request(
    handle: *mut c_void,
    role: RoutedRole,
    router_rid: Option<&RoutingId>,
    target: &ffi::zlink_routed_submit_target_t,
    parts: &mut [Message],
    timeout_ms: u32,
    userdata: *mut c_void,
) -> i32 {
    let count = parts.len();
    for (index, part) in parts.iter_mut().enumerate() {
        let final_part = index + 1 == count;
        let flag = if final_part {
            ffi::zlink_part_flag_t::ZLINK_PART_FINAL
        } else {
            ffi::zlink_part_flag_t::ZLINK_PART_MORE
        };
        let handler = final_part.then_some(routed_reply_callback as ffi::zlink_reply_handler_fn);
        let callback_data = if final_part {
            userdata
        } else {
            std::ptr::null_mut()
        };
        let rc = unsafe {
            match role {
                RoutedRole::Dealer => ffi::zlink_dealer_request_transport_pair_part(
                    handle,
                    target,
                    part.raw_mut(),
                    ffi::ZLINK_DONTWAIT,
                    flag,
                    if final_part { timeout_ms } else { 0 },
                    handler,
                    callback_data,
                ),
                RoutedRole::Router => ffi::zlink_router_request_transport_pair_part(
                    handle,
                    router_rid.expect("router target").as_raw(),
                    target.transport_pair_id,
                    target.transport_pair_generation,
                    part.raw_mut(),
                    ffi::ZLINK_DONTWAIT,
                    flag,
                    if final_part { timeout_ms } else { 0 },
                    handler,
                    callback_data,
                ),
            }
        };
        if rc != SubmitResult::Ok as i32 {
            return rc;
        }
    }
    SubmitResult::Ok as i32
}

fn duration_to_timeout_ms(duration: Duration) -> u32 {
    if duration.is_zero() {
        return 0;
    }
    duration.as_millis().clamp(1, u32::MAX as u128) as u32
}

fn timeout_submit_error() -> SubmitError {
    SubmitError::new(SubmitResult::Backpressured, libc::ETIMEDOUT)
}

fn timeout_request_error() -> RequestError {
    RequestError::new(RequestResult::TimedOut, libc::ETIMEDOUT)
}

fn request_result_from_native(result: ffi::zlink_request_result_t) -> RequestResult {
    match result {
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_OK => RequestResult::Ok,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_TIMED_OUT => RequestResult::TimedOut,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_NOT_FOUND => RequestResult::NotFound,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_TERMINATED => RequestResult::Terminated,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_PROTOCOL_ERROR => {
            RequestResult::ProtocolError
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_INTERNAL_ERROR => {
            RequestResult::InternalError
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_REJECTED => RequestResult::Rejected,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_CONFLICT => RequestResult::Conflict,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_BUSY => RequestResult::Busy,
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_NOT_CONNECTED => {
            RequestResult::NotConnected
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_INVALID_ARGUMENT => {
            RequestResult::InvalidArgument
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_INVALID_STATE => {
            RequestResult::InvalidState
        }
        ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_NOT_SUPPORTED => {
            RequestResult::NotSupported
        }
    }
}
