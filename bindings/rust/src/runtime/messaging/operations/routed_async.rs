// SPDX-License-Identifier: MPL-2.0

use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};
use std::time::Duration;

use crate::error::{RequestError, RequestResult, SubmitError, SubmitResult, ZlinkError};
use crate::ffi;
use crate::internal::{CompletionEntry, CompletionOwner, RoutedHandle};
use crate::message::{Message, RoutingId};
use crate::messaging_operations::{Empty, MessageParts, RequestOp, RequestOpStorage};
use crate::native_errors::{check_submit_rc, submit_error_from_errno};
use crate::socket::submit_part_sequence;

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
        finished: false,
    }
}

pub(crate) fn submit_routed_request_sync(
    mut operation: RequestOpStorage,
) -> Result<Vec<Message>, ZlinkError> {
    validate_request(&operation)?;
    let owner = Arc::clone(&operation.completion_owner);
    let (entry, user_context) = owner.register_request()?;
    let completion_id = match submit_request_parts(&mut operation, 0, user_context) {
        Ok(id) => id,
        Err(error) => {
            owner.unregister(user_context);
            return Err(error.into());
        }
    };
    if completion_id == 0 {
        owner.unregister(user_context);
        return Err(RequestError::new(RequestResult::InternalError, libc::EPROTO).into());
    }
    entry.publish(completion_id);
    Ok(entry.wait_request()?)
}

struct RequestFuture {
    operation: Option<RequestOpStorage>,
    entry: Option<Arc<CompletionEntry>>,
    owner: Option<Arc<CompletionOwner>>,
    context: *mut std::ffi::c_void,
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
            let mut operation = self.operation.take().expect("active request");
            if let Err(error) = validate_request(&operation) {
                self.finished = true;
                return Poll::Ready(Err(error.into()));
            }
            let owner = Arc::clone(&operation.completion_owner);
            let (entry, user_context) = match owner.register_request() {
                Ok(registered) => registered,
                Err(error) => {
                    self.finished = true;
                    return Poll::Ready(Err(error.into()));
                }
            };
            match submit_request_parts(&mut operation, ffi::ZLINK_DONTWAIT, user_context) {
                Ok(0) => {
                    owner.unregister(user_context);
                    self.finished = true;
                    return Poll::Ready(Err(RequestError::new(
                        RequestResult::InternalError,
                        libc::EPROTO,
                    )
                    .into()));
                }
                Ok(id) => entry.publish(id),
                Err(error) => {
                    owner.unregister(user_context);
                    self.finished = true;
                    return Poll::Ready(Err(error.into()));
                }
            }
            self.owner = Some(owner);
            self.entry = Some(entry);
            self.context = user_context;
        }

        let outcome = self
            .entry
            .as_ref()
            .expect("request completion")
            .poll_request(cx.waker());
        match outcome {
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

impl Drop for RequestFuture {
    fn drop(&mut self) {
        if !self.finished {
            if let Some(entry) = &self.entry {
                let captured = entry.detach();
                if captured {
                    if let Some(owner) = &self.owner {
                        owner.unregister(self.context);
                    }
                }
            }
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

fn submit_request_parts(
    operation: &mut RequestOpStorage,
    flags: u32,
    user_context: *mut std::ffi::c_void,
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
