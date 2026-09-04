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
use crate::messaging_operations::{Empty, MessageParts, RequestOp, RequestOpStorage};
use crate::native_errors::{check_submit_rc, submit_error_from_errno};
use crate::socket::submit_part_sequence;

use super::send_ops::submit_shared_part_sequence;

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
    operation: RequestOpStorage,
) -> impl Future<Output = Result<Vec<Message>, ZlinkError>> + Send {
    RequestFuture {
        operation: Some(operation),
        entry: None,
        owner: None,
        context: std::ptr::null_mut(),
        waiting_for_writable: false,
        finished: false,
    }
}

pub(crate) fn submit_routed_request_sync(
    mut operation: RequestOpStorage,
) -> Result<Vec<Message>, ZlinkError> {
    validate_request(&operation)?;
    let owner = Arc::clone(&operation.completion_owner);
    let target = operation.peer_rid;
    let (entry, user_context) = owner.register_request(target)?;
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
/// stack-local Core shared message descriptors. A refusal arms exactly one
/// WRITABLE token; only that token permits the next attempt. Once admitted,
/// the same entry changes phase and waits for the REQUEST reply or terminal.
struct RequestFuture {
    operation: Option<RequestOpStorage>,
    entry: Option<Arc<CompletionEntry>>,
    owner: Option<Arc<CompletionOwner>>,
    context: *mut c_void,
    waiting_for_writable: bool,
    finished: bool,
}

unsafe impl Send for RequestFuture {}

impl Future for RequestFuture {
    type Output = Result<Vec<Message>, ZlinkError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.finished {
            panic!("request Future polled after completion");
        }
        if self.entry.is_none() {
            let operation = self.operation.as_ref().expect("active request");
            if let Err(error) = validate_request(operation) {
                return self.finish(Err(error.into()));
            }
            let owner = Arc::clone(&operation.completion_owner);
            let target = operation.peer_rid;
            let (entry, user_context) = match owner.register_request(target) {
                Ok(registered) => registered,
                Err(error) => return self.finish(Err(error.into())),
            };
            self.owner = Some(owner);
            self.entry = Some(entry);
            self.context = user_context;
        }

        loop {
            if self.waiting_for_writable {
                let entry = self.entry.as_ref().expect("request retry entry");
                match entry.poll_writable(cx.waker()) {
                    Poll::Ready(Ok(())) => self.waiting_for_writable = false,
                    Poll::Ready(Err(error)) => return self.finish(Err(error.into())),
                    Poll::Pending => return Poll::Pending,
                }
            }

            if self.operation.is_some() {
                let context = self.context;
                let entry = Arc::clone(self.entry.as_ref().expect("request completion entry"));
                let attempt = {
                    let operation = self.operation.as_mut().expect("active request");
                    submit_request_attempt(operation, &entry, context)
                };
                match attempt {
                    Ok(RequestAttempt::Admitted) => {
                        self.operation.take();
                    }
                    Ok(RequestAttempt::Waiting) => {
                        self.waiting_for_writable = true;
                        continue;
                    }
                    Err(failure) if failure.live_token => {
                        return self.finish_detached(Err(failure.error.into()));
                    }
                    Err(failure) => return self.finish(Err(failure.error.into())),
                }
            }

            let outcome = self
                .entry
                .as_ref()
                .expect("request completion")
                .poll_request(cx.waker());
            return match outcome {
                Poll::Ready(Ok(parts)) => self.finish(Ok(parts)),
                Poll::Ready(Err(error)) => self.finish(Err(error.into())),
                Poll::Pending => Poll::Pending,
            };
        }
    }
}

impl RequestFuture {
    fn finish(
        &mut self,
        result: Result<Vec<Message>, ZlinkError>,
    ) -> Poll<Result<Vec<Message>, ZlinkError>> {
        if self.entry.take().is_some() {
            if let Some(owner) = &self.owner {
                owner.unregister(self.context);
            }
        }
        self.operation.take();
        self.waiting_for_writable = false;
        self.finished = true;
        Poll::Ready(result)
    }

    fn finish_detached(
        &mut self,
        result: Result<Vec<Message>, ZlinkError>,
    ) -> Poll<Result<Vec<Message>, ZlinkError>> {
        self.detach_entry();
        self.operation.take();
        self.waiting_for_writable = false;
        self.finished = true;
        Poll::Ready(result)
    }

    fn detach_entry(&mut self) {
        if let Some(entry) = self.entry.take() {
            if entry.detach() {
                if let Some(owner) = &self.owner {
                    owner.unregister(self.context);
                }
            }
        }
    }
}

impl Drop for RequestFuture {
    fn drop(&mut self) {
        if !self.finished {
            self.detach_entry();
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
            let (rc, errno) = submit_shared_part_sequence(
                &mut operation.parts,
                |part, part_flag, is_final| unsafe {
                    ffi::zlink_request_part(
                        handle,
                        target,
                        part,
                        ffi::ZLINK_DONTWAIT,
                        part_flag,
                        if is_final { timeout_ms } else { 0 },
                        if is_final {
                            user_context
                        } else {
                            std::ptr::null_mut()
                        },
                        if is_final {
                            &mut completion_id
                        } else {
                            std::ptr::null_mut()
                        },
                    )
                },
            )?;

            if rc == SubmitResult::Ok as i32 && completion_id != 0 {
                entry.publish_request(completion_id);
                return Ok(Ok(RequestAttempt::Admitted));
            }
            if rc == SubmitResult::Backpressured as i32
                && errno == libc::EAGAIN
                && completion_id != 0
            {
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
            if rc == SubmitResult::Ok as i32 || errno == libc::EAGAIN {
                return Ok(Err(RequestAttemptError::without_token(SubmitError::new(
                    SubmitResult::InternalError,
                    libc::EPROTO,
                ))));
            }
            Ok(Err(RequestAttemptError::without_token(
                submit_error_from_errno(errno),
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
    let rc = submit_part_sequence(&mut operation.parts, |part, part_flag, is_final| unsafe {
        ffi::zlink_request_part(
            handle,
            target,
            part,
            flags,
            part_flag,
            if is_final { timeout_ms } else { 0 },
            if is_final {
                user_context
            } else {
                std::ptr::null_mut()
            },
            if is_final {
                &mut completion_id
            } else {
                std::ptr::null_mut()
            },
        )
    })?;
    check_submit_rc(rc)?;
    Ok(completion_id)
}

fn duration_to_timeout_ms(duration: Duration) -> u32 {
    if duration.is_zero() {
        0
    } else {
        duration.as_millis().clamp(1, u32::MAX as u128) as u32
    }
}
