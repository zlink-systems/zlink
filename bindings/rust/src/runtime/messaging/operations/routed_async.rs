// SPDX-License-Identifier: MPL-2.0

use std::ffi::c_void;
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};
use std::time::Duration;

use crate::error::{SubmitError, SubmitResult, ZlinkError};
use crate::ffi;
use crate::internal::{CompletionEntry, CompletionOwner, RoutedHandle};
use crate::message::{Message, RoutingId};
use crate::messaging_operations::{
    Empty, MessageParts, RequestOp, RequestOpStorage, RequestSubmission,
};
use crate::native_errors::{submit_error_from_errno, submit_error_from_rc};

use super::send_ops::{check_submit_result, submit_shared_message};

pub(crate) fn dealer_request_op(
    routed: Arc<RoutedHandle>,
    completion_owner: Arc<CompletionOwner>,
) -> RequestOp<Empty> {
    RequestOp {
        inner: RequestOpStorage {
            routed,
            completion_owner,
            peer_rid: None,
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn router_request_op(
    routed: Arc<RoutedHandle>,
    completion_owner: Arc<CompletionOwner>,
    peer_rid: RoutingId,
) -> RequestOp<Empty> {
    RequestOp {
        inner: RequestOpStorage {
            routed,
            completion_owner,
            peer_rid: Some(peer_rid),
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn submit_routed_request(
    mut operation: RequestOpStorage,
) -> Result<RequestSubmission, ZlinkError> {
    validate_request(&operation)?;
    let owner = Arc::clone(&operation.completion_owner);
    let (entry, context) = owner.register_request()?;
    match submit_request_attempt(&mut operation, &entry, context) {
        Ok(RequestAttempt::Admitted) => {
            entry.admission_succeeded();
            Ok(RequestSubmission {
                result: SubmitResult::Ok,
                admitted: Box::pin(std::future::ready(Ok(()))),
                reply: Box::pin(RequestReplyFuture::new(entry, owner, context)),
            })
        }
        Ok(RequestAttempt::Waiting) => Ok(RequestSubmission {
            result: SubmitResult::Backpressured,
            admitted: Box::pin(RequestAdmissionFuture {
                operation: Some(operation),
                entry: Arc::clone(&entry),
                owner: Arc::clone(&owner),
                context,
                waiting_for_writable: true,
                finished: false,
            }),
            reply: Box::pin(RequestReplyFuture::new(entry, owner, context)),
        }),
        Err(failure) => {
            if failure.live_token {
                if entry.detach() {
                    owner.unregister(context);
                }
            } else {
                owner.unregister(context);
            }
            Err(failure.error.into())
        }
    }
}

pub(crate) fn submit_routed_request_sync(
    mut operation: RequestOpStorage,
) -> Result<Vec<Message>, ZlinkError> {
    validate_request(&operation)?;
    let owner = Arc::clone(&operation.completion_owner);
    let (entry, user_context) = owner.register_request()?;
    let submission = owner.with_submit(|| {
        let completion_id = submit_request_parts(&mut operation, 0, user_context)?;
        if completion_id == 0 {
            return Err(SubmitError::new(SubmitResult::InternalError, libc::EPROTO));
        }
        entry.publish_request(completion_id);
        Ok(())
    });
    if let Err(error) = submission {
        owner.unregister(user_context);
        return Err(error.into());
    }
    Ok(entry.wait_request()?)
}

/// Managed DONTWAIT REQUEST.
///
/// The builder-owned packet stays intact while each admission attempt uses
/// stack-local Core shared message descriptors. A refusal with a nonzero token
/// arms a WRITABLE wait; only that token permits the next attempt. Once admitted,
/// the same entry changes phase and waits for the REQUEST reply or terminal.
struct RequestAdmissionFuture {
    operation: Option<RequestOpStorage>,
    entry: Arc<CompletionEntry>,
    owner: Arc<CompletionOwner>,
    context: *mut c_void,
    waiting_for_writable: bool,
    finished: bool,
}

unsafe impl Send for RequestAdmissionFuture {}

impl Future for RequestAdmissionFuture {
    type Output = Result<(), SubmitError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.finished {
            panic!("request admission Future polled after completion");
        }

        loop {
            if self.waiting_for_writable {
                match self.entry.poll_writable(cx.waker()) {
                    Poll::Ready(Ok(())) => self.waiting_for_writable = false,
                    Poll::Ready(Err(error)) => return self.finish(Err(error)),
                    Poll::Pending => return Poll::Pending,
                }
            }

            let context = self.context;
            let entry = Arc::clone(&self.entry);
            let attempt = {
                let operation = self.operation.as_mut().expect("active request");
                submit_request_attempt(operation, &entry, context)
            };
            match attempt {
                Ok(RequestAttempt::Admitted) => {
                    self.operation.take();
                    self.entry.admission_succeeded();
                    self.finished = true;
                    return Poll::Ready(Ok(()));
                }
                Ok(RequestAttempt::Waiting) => {
                    self.waiting_for_writable = true;
                }
                Err(failure) => return self.finish(Err(failure.error)),
            }
        }
    }
}

impl RequestAdmissionFuture {
    fn finish(&mut self, result: Result<(), SubmitError>) -> Poll<Result<(), SubmitError>> {
        if let Err(error) = result {
            if self.entry.fail_admission(error) {
                self.owner.unregister(self.context);
            }
        }
        self.operation.take();
        self.waiting_for_writable = false;
        self.finished = true;
        Poll::Ready(result)
    }
}

impl Drop for RequestAdmissionFuture {
    fn drop(&mut self) {
        if !self.finished {
            let error = SubmitError::new(SubmitResult::Terminated, libc::ECANCELED);
            if self.entry.fail_admission(error) {
                self.owner.unregister(self.context);
            }
        }
    }
}

struct RequestReplyFuture {
    entry: Arc<CompletionEntry>,
    owner: Arc<CompletionOwner>,
    context: *mut c_void,
    finished: bool,
}

unsafe impl Send for RequestReplyFuture {}

impl RequestReplyFuture {
    fn new(entry: Arc<CompletionEntry>, owner: Arc<CompletionOwner>, context: *mut c_void) -> Self {
        Self {
            entry,
            owner,
            context,
            finished: false,
        }
    }
}

impl Future for RequestReplyFuture {
    type Output = Result<Vec<Message>, ZlinkError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.finished {
            panic!("request reply Future polled after completion");
        }
        match self.entry.poll_request(cx.waker()) {
            Poll::Ready(result) => {
                self.finished = true;
                Poll::Ready(result)
            }
            Poll::Pending => Poll::Pending,
        }
    }
}

impl Drop for RequestReplyFuture {
    fn drop(&mut self) {
        if !self.finished && self.entry.detach_reply() {
            self.owner.unregister(self.context);
        }
    }
}

enum RequestAttempt {
    Admitted,
    Waiting,
}

struct RequestAttemptError {
    error: SubmitError,
    live_token: bool,
}

impl RequestAttemptError {
    fn without_token(error: SubmitError) -> Self {
        Self {
            error,
            live_token: false,
        }
    }
}

fn validate_request(operation: &RequestOpStorage) -> Result<(), SubmitError> {
    if operation.parts.is_empty() {
        Err(SubmitError::new(
            SubmitResult::InvalidArgument,
            libc::EINVAL,
        ))
    } else if operation.routed.handle().is_null() {
        Err(submit_error_from_errno(libc::ECANCELED))
    } else {
        Ok(())
    }
}

fn submit_request_attempt(
    operation: &mut RequestOpStorage,
    entry: &CompletionEntry,
    user_context: *mut c_void,
) -> Result<RequestAttempt, RequestAttemptError> {
    let target = operation
        .peer_rid
        .as_ref()
        .map_or(std::ptr::null(), |rid| rid.as_raw() as *const _);
    let timeout_ms = duration_to_timeout_ms(operation.timeout);
    let mut completion_id = 0;
    let owner = Arc::clone(&operation.completion_owner);
    owner
        .with_submit(|| {
            let handle = operation.routed.handle();
            if handle.is_null() {
                return Err(submit_error_from_errno(libc::ECANCELED));
            }
            let (rc, errno) = submit_shared_message(&mut operation.parts, |parts, count| unsafe {
                ffi::zlink_request(
                    handle,
                    target,
                    parts,
                    count,
                    ffi::ZLINK_DONTWAIT,
                    timeout_ms,
                    user_context,
                    &mut completion_id,
                )
            })?;

            if rc == SubmitResult::Ok as i32 && completion_id != 0 {
                entry.publish_request(completion_id);
                return Ok(Ok(RequestAttempt::Admitted));
            }
            if rc == SubmitResult::Backpressured as i32 && completion_id != 0 {
                entry.publish_writable(completion_id);
                return Ok(Ok(RequestAttempt::Waiting));
            }
            if completion_id != 0 {
                entry.publish_writable(completion_id);
                return Ok(Err(RequestAttemptError {
                    error: SubmitError::new(SubmitResult::InternalError, libc::EPROTO),
                    live_token: true,
                }));
            }
            if rc == SubmitResult::Ok as i32 {
                return Ok(Err(RequestAttemptError::without_token(SubmitError::new(
                    SubmitResult::InternalError,
                    libc::EPROTO,
                ))));
            }
            Ok(Err(RequestAttemptError::without_token(
                submit_error_from_rc(rc, errno),
            )))
        })
        .map_err(RequestAttemptError::without_token)?
}

fn submit_request_parts(
    operation: &mut RequestOpStorage,
    flags: u32,
    user_context: *mut c_void,
) -> Result<u64, SubmitError> {
    let handle = operation.routed.handle();
    let target = operation
        .peer_rid
        .as_ref()
        .map_or(std::ptr::null(), |rid| rid.as_raw() as *const _);
    let timeout_ms = duration_to_timeout_ms(operation.timeout);
    let mut completion_id = 0;
    let (rc, errno) = submit_shared_message(&mut operation.parts, |parts, count| unsafe {
        ffi::zlink_request(
            handle,
            target,
            parts,
            count,
            flags,
            timeout_ms,
            user_context,
            &mut completion_id,
        )
    })?;
    check_submit_result(rc, errno)?;
    Ok(completion_id)
}

fn duration_to_timeout_ms(duration: Duration) -> u32 {
    if duration.is_zero() {
        0
    } else {
        duration.as_millis().clamp(1, u32::MAX as u128) as u32
    }
}
