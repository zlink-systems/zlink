// SPDX-License-Identifier: MPL-2.0

//! HWM-managed request terminal.
//!
//! Core already owns the point that drives a request to completion: the reply
//! handler callback and `ZLINK_REQUEST_TIMED_OUT`. The binding only wires that
//! point to the `Future`'s completion. It owns no retry queue, no deadline
//! timer and no thread, and the `Future` resumes in whatever context Core
//! delivered the reply on.

use std::ffi::c_void;
use std::future::Future;
use std::marker::PhantomData;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Waker};
use std::time::Duration;

use crate::error::{RequestError, RequestResult, SubmitError, SubmitResult, ZlinkError};
use crate::ffi;
use crate::internal::{RoutedHandle, RoutedRole};
use crate::message::{Message, RoutingId};
use crate::messaging_operations::{
    Empty, MessageParts, RequestOp, RequestOpStorage,
};
use crate::native_errors::{request_error_from_result, submit_error_from_errno};

type RequestCallback = Box<dyn FnOnce(Result<Vec<Message>, ZlinkError>) + Send + 'static>;

pub(crate) fn dealer_request_op(routed: Arc<RoutedHandle>) -> RequestOp<Empty> {
    RequestOp {
        inner: RequestOpStorage {
            routed,
            peer_rid: None,
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
        },
        _state: PhantomData,
    }
}

pub(crate) fn router_request_op(
    routed: Arc<RoutedHandle>,
    peer_rid: RoutingId,
) -> RequestOp<Empty> {
    RequestOp {
        inner: RequestOpStorage {
            routed,
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

pub(crate) fn submit_routed_request_sync(
    operation: RequestOpStorage,
    flags: crate::flags::SendFlags,
) -> Result<Vec<Message>, ZlinkError> {
    let (sender, receiver) = std::sync::mpsc::sync_channel(1);
    submit_routed_request_callback(operation, flags, move |outcome| {
        let _ = sender.send(outcome);
    })?;
    receiver.recv().map_err(|_| ZlinkError::Request(RequestError::new(
        RequestResult::Terminated,
        libc::ECANCELED,
    )))?
}

pub(crate) fn submit_routed_request_callback<F>(
    operation: RequestOpStorage,
    flags: crate::flags::SendFlags,
    callback: F,
) -> Result<(), SubmitError>
where
    F: FnOnce(Result<Vec<Message>, ZlinkError>) + Send + 'static,
{
    submit_request_callback(operation, flags.bits(), Box::new(callback))
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

/// Consumer side of one Core-owned request.
///
/// The first poll performs exactly one submit. DEALER target selection stays
/// inside that Core submit, while ROUTER preserves its explicit exact target.
/// Core owns the HWM contract and the reply deadline; on acceptance the Core
/// reply callback completes this Future.
struct RoutedRequestFuture {
    operation: Option<RequestOpStorage>,
    completion: Arc<RequestCompletion>,
    accepted: bool,
    finished: bool,
}

impl RoutedRequestFuture {
    fn new(operation: RequestOpStorage) -> Self {
        Self {
            operation: Some(operation),
            completion: RequestCompletion::new(),
            accepted: false,
            finished: false,
        }
    }

    fn start(&mut self) -> Result<(), ZlinkError> {
        let operation = self.operation.take().expect("active routed request");
        if operation.parts.is_empty() {
            return Err(SubmitError::new(SubmitResult::InvalidArgument, libc::EINVAL).into());
        }

        let timeout = if operation.timeout.is_zero() {
            operation.routed.request_timeout()?
        } else {
            operation.timeout
        };
        let timeout_ms = duration_to_timeout_ms(timeout);

        let role = operation.routed.role();
        let target = select_request_target(&operation.routed, operation.peer_rid.as_ref())?;

        let mut parts = operation.parts.into_vec();
        let callback_owner = Arc::into_raw(Arc::clone(&self.completion));
        let result = operation.routed.with_live_handle(|handle| {
            submit_request(
                handle,
                role,
                operation.peer_rid.as_ref(),
                target.as_ref(),
                &mut parts,
                timeout_ms,
                callback_owner.cast_mut().cast(),
            )
        });
        let rc = match result {
            Some(rc) => rc,
            None => {
                unsafe { drop(Arc::from_raw(callback_owner)) };
                return Err(submit_error_from_errno(libc::ECANCELED).into());
            }
        };
        if rc != SubmitResult::Ok as i32 {
            unsafe { drop(Arc::from_raw(callback_owner)) };
            let errno = unsafe { ffi::zlink_errno() };
            // Core reports its own request-domain outcomes through the same
            // submit result; keep the timeout / cancel detail in that domain.
            if errno == libc::ETIMEDOUT {
                return Err(timeout_request_error().into());
            }
            return Err(submit_error_from_errno(errno).into());
        }
        // Core owns the request from here: the reply callback completes it.
        self.accepted = true;
        Ok(())
    }
}

impl Future for RoutedRequestFuture {
    type Output = Result<Vec<Message>, ZlinkError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.finished {
            panic!("routed request Future polled after completion");
        }
        if !self.accepted {
            if let Err(error) = self.start() {
                self.finished = true;
                return Poll::Ready(Err(error));
            }
        }
        match self.completion.poll(cx.waker()) {
            Poll::Ready(Ok(parts)) => {
                self.finished = true;
                Poll::Ready(Ok(parts))
            }
            Poll::Ready(Err(error)) => {
                self.finished = true;
                Poll::Ready(Err(error.into()))
            }
            Poll::Pending => Poll::Pending,
        }
    }
}

impl Drop for RoutedRequestFuture {
    fn drop(&mut self) {
        if self.accepted && !self.finished {
            // The request is Core's; only the consumer detaches. Core still
            // completes the reply or the timeout exactly once.
            self.completion.detach();
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

unsafe extern "C" fn routed_reply_callback_fn(
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
    let callback = unsafe { Box::from_raw(userdata.cast::<RequestCallback>()) };
    let received = crate::socket::take_parts(parts, part_count);
    let outcome = if result == ffi::zlink_request_result_t::ZLINK_REQUEST_RESULT_OK {
        Ok(received)
    } else {
        drop(received);
        Err(request_error_from_result(request_result_from_native(result)).into())
    };
    callback(outcome);
}

fn submit_request_callback(
    operation: RequestOpStorage,
    flags: u32,
    callback: RequestCallback,
) -> Result<(), SubmitError> {
    if operation.parts.is_empty() {
        return Err(SubmitError::new(SubmitResult::InvalidArgument, libc::EINVAL));
    }
    let timeout = if operation.timeout.is_zero() {
        operation.routed.request_timeout()?
    } else {
        operation.timeout
    };
    let role = operation.routed.role();
    let target = select_request_target(&operation.routed, operation.peer_rid.as_ref())?;

    let mut parts = operation.parts.into_vec();
    let callback_owner = Box::into_raw(Box::new(callback)).cast::<c_void>();
    let result = operation.routed.with_live_handle(|handle| {
        submit_request_with(
            handle,
            role,
            operation.peer_rid.as_ref(),
            target.as_ref(),
            &mut parts,
            flags,
            duration_to_timeout_ms(timeout),
            routed_reply_callback_fn,
            callback_owner,
        )
    });
    let rc = result.unwrap_or_else(|| {
        unsafe { drop(Box::from_raw(callback_owner.cast::<RequestCallback>())) };
        SubmitResult::Terminated as i32
    });
    if rc != SubmitResult::Ok as i32 {
        if result.is_some() {
            unsafe { drop(Box::from_raw(callback_owner.cast::<RequestCallback>())) };
        }
        return Err(submit_error_from_errno(if result.is_some() {
            unsafe { ffi::zlink_errno() }
        } else {
            libc::ECANCELED
        }));
    }
    Ok(())
}

fn select_request_target(
    routed: &RoutedHandle,
    router_rid: Option<&RoutingId>,
) -> Result<Option<ffi::zlink_routed_submit_target_t>, SubmitError> {
    if routed.role() == RoutedRole::Dealer {
        return Ok(None);
    }
    let router_rid =
        router_rid.ok_or_else(|| SubmitError::new(SubmitResult::InvalidArgument, libc::EINVAL))?;
    let (rc, target) = routed.select_target(Some(router_rid));
    if rc != 0 {
        return Err(submit_error_from_errno(unsafe { ffi::zlink_errno() }));
    }
    Ok(Some(target))
}

fn submit_request(
    handle: *mut c_void,
    role: RoutedRole,
    router_rid: Option<&RoutingId>,
    target: Option<&ffi::zlink_routed_submit_target_t>,
    parts: &mut [Message],
    timeout_ms: u32,
    userdata: *mut c_void,
) -> i32 {
    submit_request_with(
        handle,
        role,
        router_rid,
        target,
        parts,
        0,
        timeout_ms,
        routed_reply_callback,
        userdata,
    )
}

fn submit_request_with(
    handle: *mut c_void,
    role: RoutedRole,
    router_rid: Option<&RoutingId>,
    target: Option<&ffi::zlink_routed_submit_target_t>,
    parts: &mut [Message],
    flags: u32,
    timeout_ms: u32,
    callback: ffi::zlink_reply_handler_fn,
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
        let handler = final_part.then_some(callback);
        let callback_data = if final_part {
            userdata
        } else {
            std::ptr::null_mut()
        };
        // Core owns the HWM contract for this submit: it waits internally,
        // bounds the wait with SNDTIMEO, and reports the outcome once.
        let rc = unsafe {
            match role {
                RoutedRole::Dealer => ffi::zlink_dealer_request_part(
                    handle,
                    part.raw_mut(),
                    flags,
                    flag,
                    if final_part { timeout_ms } else { 0 },
                    handler,
                    callback_data,
                ),
                RoutedRole::Router => {
                    let target = target.expect("router transport pair");
                    ffi::zlink_router_request_transport_pair_part(
                        handle,
                        router_rid.expect("router target").as_raw(),
                        target.transport_pair_id,
                        target.transport_pair_generation,
                        part.raw_mut(),
                        flags,
                        flag,
                        if final_part { timeout_ms } else { 0 },
                        handler,
                        callback_data,
                    )
                }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dealer_request_target_selection_is_core_owned() {
        let routed = RoutedHandle::new(std::ptr::null_mut(), RoutedRole::Dealer);
        assert!(select_request_target(&routed, None).unwrap().is_none());

        let source = include_str!("routed_async.rs");
        let runtime_source = source.split("#[cfg(test)]").next().unwrap();
        let dealer_standard = ["ffi::zlink_dealer_request_", "part("].concat();
        let dealer_exact = ["ffi::zlink_dealer_request_", "transport_pair_part("].concat();
        let router_exact = ["ffi::zlink_router_request_", "transport_pair_part("].concat();
        assert!(runtime_source.contains(&dealer_standard));
        assert!(!runtime_source.contains(&dealer_exact));
        assert!(runtime_source.contains(&router_exact));
    }
}
