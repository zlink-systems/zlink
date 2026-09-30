// SPDX-License-Identifier: MPL-2.0

use std::ffi::c_void;
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};

use crate::error::{SubmitError, SubmitResult};
use crate::ffi;
use crate::internal::{CompletionEntry, CompletionOwner, RoutedHandle};
use crate::messaging_operations::{
    Empty, MessageParts, PublishOp, PublishOpStorage, SendOp, SendOpStorage, SendSubmission,
};
use crate::native_errors::submit_error_from_rc;

pub(crate) fn socket_send_op(
    handle: *mut c_void,
    completion_owner: Arc<CompletionOwner>,
) -> SendOp<Empty> {
    SendOp {
        inner: SendOpStorage {
            handle,
            routed: None,
            completion_owner,
            target: None,
            parts: MessageParts::default(),
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn dealer_send_op(
    routed: Arc<RoutedHandle>,
    completion_owner: Arc<CompletionOwner>,
) -> SendOp<Empty> {
    SendOp {
        inner: SendOpStorage {
            handle: routed.handle(),
            routed: Some(routed),
            completion_owner,
            target: None,
            parts: MessageParts::default(),
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn routed_send_op(
    routed: Arc<RoutedHandle>,
    completion_owner: Arc<CompletionOwner>,
    target: crate::RoutingId,
) -> SendOp<Empty> {
    SendOp {
        inner: SendOpStorage {
            handle: routed.handle(),
            routed: Some(routed),
            completion_owner,
            target: Some(target),
            parts: MessageParts::default(),
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn stream_send_to_op(
    handle: *mut c_void,
    completion_owner: Arc<CompletionOwner>,
    target: crate::RoutingId,
) -> SendOp<Empty> {
    SendOp {
        inner: SendOpStorage {
            handle,
            routed: None,
            completion_owner,
            target: Some(target),
            parts: MessageParts::default(),
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn socket_publish_op(handle: *mut c_void, topic: smol_str::SmolStr) -> PublishOp<Empty> {
    PublishOp {
        inner: PublishOpStorage {
            handle,
            topic,
            parts: MessageParts::default(),
            flags: crate::SendFlags::NONE,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn submit_publish(mut op: PublishOpStorage) -> Result<(), SubmitError> {
    let flags = op.flags.bits();
    let mut topic_buf = [0u8; 256];
    let bytes = op.topic.as_str().as_bytes();
    let mut long_topic_buf = Vec::new();
    let topic_ptr = if bytes.len() < topic_buf.len() {
        topic_buf[..bytes.len()].copy_from_slice(bytes);
        topic_buf.as_ptr().cast()
    } else {
        long_topic_buf.reserve_exact(bytes.len() + 1);
        long_topic_buf.extend_from_slice(bytes);
        long_topic_buf.push(0);
        long_topic_buf.as_ptr().cast()
    };
    let (rc, errno) = submit_shared_message(&mut op.parts, |parts, count| unsafe {
        ffi::zlink_publish(op.handle, topic_ptr, parts, count, flags)
    })?;
    check_submit_result(rc, errno)
}

pub(crate) fn submit_send(mut op: SendOpStorage) -> Result<SendSubmission, SubmitError> {
    if op.parts.is_empty() {
        return Err(SubmitError::new(
            SubmitResult::InvalidArgument,
            libc::EINVAL,
        ));
    }
    live_handle(&op)?;

    let owner = Arc::clone(&op.completion_owner);
    let context = owner.next_context();
    // The entry exists before the native call, so a WRITABLE record pulled by
    // the drain before the submit returns joins the token inside the entry.
    let entry = owner.register_send(context)?;
    match submit_send_attempt(&mut op, context) {
        Ok(SendAttempt::Admitted) => {
            owner.unregister(context);
            Ok(SendSubmission {
                result: SubmitResult::Ok,
                admitted: Box::pin(std::future::ready(Ok(()))),
            })
        }
        Ok(SendAttempt::Waiting(completion_id)) => {
            if let Err(error) = owner.publish_send_token(&entry, completion_id) {
                if entry.detach() {
                    owner.unregister(context);
                }
                return Err(error);
            }
            Ok(SendSubmission {
                result: SubmitResult::Backpressured,
                admitted: Box::pin(SendFuture {
                    operation: Some(op),
                    context,
                    entry: Some(entry),
                    waiting_for_writable: true,
                    finished: false,
                }),
            })
        }
        Err(failure) => {
            let removable = match failure.live_token {
                Some(completion_id) => {
                    let _ = owner.publish_send_token(&entry, completion_id);
                    entry.detach()
                }
                None => true,
            };
            if removable {
                owner.unregister(context);
            }
            Err(failure.error)
        }
    }
}

pub(crate) fn submit_send_blocking(mut op: SendOpStorage) -> Result<(), SubmitError> {
    let owner = Arc::clone(&op.completion_owner);
    owner.ensure_open()?;
    let handle = live_handle(&op)?;
    let target: *const ffi::zlink_routing_id_t = op
        .target
        .as_ref()
        .map_or(std::ptr::null(), |rid| rid.as_raw() as *const _);
    let (rc, errno) = submit_owned_message(&mut op.parts, |parts, count| unsafe {
        if target.is_null() {
            ffi::zlink_send(
                handle,
                parts,
                count,
                0,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            )
        } else {
            ffi::zlink_send_rid(
                handle,
                target,
                parts,
                count,
                0,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            )
        }
    })?;
    check_submit_result(rc, errno)
}

/// Managed DONTWAIT SEND.
///
/// The packet stays owned here. Each attempt submits Core shared copies of the
/// parts; when Core answers with a wait token the entry is registered with the
/// socket's completion owner and the Future parks until the WRITABLE record
/// for exactly that token (same context, same routed target) is drained by the
/// public poller that owns the queue.
struct SendFuture {
    operation: Option<SendOpStorage>,
    context: *mut c_void,
    entry: Option<Arc<CompletionEntry>>,
    waiting_for_writable: bool,
    finished: bool,
}

unsafe impl Send for SendFuture {}

impl Future for SendFuture {
    type Output = Result<(), SubmitError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.finished {
            panic!("send Future polled after completion");
        }
        loop {
            if self.waiting_for_writable {
                let entry = self.entry.as_ref().expect("send retry entry");
                match entry.poll_writable(cx.waker()) {
                    Poll::Ready(Ok(())) => self.waiting_for_writable = false,
                    Poll::Ready(Err(error)) => return self.finish(Err(error)),
                    Poll::Pending => return Poll::Pending,
                }
            }

            let attempt = self.submit_attempt();
            match attempt {
                Ok(SendAttempt::Admitted) => return self.finish(Ok(())),
                Ok(SendAttempt::Waiting(_)) => self.waiting_for_writable = true,
                Err(failure) => {
                    if failure.live_token.is_some() {
                        return self.finish_detached(Err(failure.error));
                    }
                    return self.finish(Err(failure.error));
                }
            }
        }
    }
}

impl SendFuture {
    /// Retries the native submit. The entry stays registered under the stable
    /// context, so a record pulled before the retry returns joins the token
    /// published here.
    fn submit_attempt(&mut self) -> Result<SendAttempt, SendAttemptError> {
        self.operation
            .as_ref()
            .expect("active send")
            .completion_owner
            .ensure_open()?;
        let context = self.context;
        let attempt = {
            let operation = self.operation.as_mut().expect("active send");
            submit_send_attempt(operation, context)
        };
        match attempt {
            Ok(SendAttempt::Waiting(completion_id)) => {
                self.publish(completion_id)
                    .map_err(SendAttemptError::without_token)?;
                Ok(SendAttempt::Waiting(completion_id))
            }
            Err(failure) => {
                if let Some(completion_id) = failure.live_token {
                    let _ = self.publish(completion_id);
                }
                Err(failure)
            }
            admitted => admitted,
        }
    }

    /// Publishes the wait token Core just issued for the registered entry.
    fn publish(&self, completion_id: u64) -> Result<(), SubmitError> {
        let operation = self.operation.as_ref().expect("active send");
        let entry = self.entry.as_ref().expect("send retry entry");
        operation
            .completion_owner
            .publish_send_token(entry, completion_id)
    }

    fn finish(&mut self, result: Result<(), SubmitError>) -> Poll<Result<(), SubmitError>> {
        if self.entry.take().is_some() {
            if let Some(operation) = &self.operation {
                operation.completion_owner.unregister(self.context);
            }
        }
        self.operation.take();
        self.waiting_for_writable = false;
        self.finished = true;
        Poll::Ready(result)
    }

    /// Finishes the Rust waiter while retaining its stable opaque context in
    /// the registry until Core retires the already-issued token.
    fn finish_detached(
        &mut self,
        result: Result<(), SubmitError>,
    ) -> Poll<Result<(), SubmitError>> {
        self.detach_entry();
        self.operation.take();
        self.waiting_for_writable = false;
        self.finished = true;
        Poll::Ready(result)
    }

    fn detach_entry(&mut self) {
        if let Some(entry) = self.entry.take() {
            if entry.detach() {
                if let Some(operation) = &self.operation {
                    operation.completion_owner.unregister(self.context);
                }
            }
        }
    }
}

impl Drop for SendFuture {
    fn drop(&mut self) {
        if !self.finished {
            self.detach_entry();
        }
    }
}

enum SendAttempt {
    Admitted,
    Waiting(u64),
}

struct SendAttemptError {
    error: SubmitError,
    live_token: Option<u64>,
}

impl SendAttemptError {
    fn without_token(error: SubmitError) -> Self {
        Self {
            error,
            live_token: None,
        }
    }
}

impl From<SubmitError> for SendAttemptError {
    fn from(error: SubmitError) -> Self {
        Self::without_token(error)
    }
}

/// Submits one whole-message array built from shared copies of `parts`.
/// Core consumes every array slot on every result; the builder-owned packet is
/// retained so completion-backed operations can rebuild and retry the record.
pub(super) fn submit_shared_message(
    parts: &mut MessageParts,
    submit: impl FnOnce(*mut ffi::zlink_msg_t, usize) -> i32,
) -> Result<(i32, i32), SubmitError> {
    if parts.is_empty() {
        return Err(SubmitError::new(
            SubmitResult::InvalidArgument,
            libc::EINVAL,
        ));
    }

    let mut native_parts = Vec::with_capacity(parts.len());
    for part in parts.iter_mut() {
        let mut attempt = std::mem::MaybeUninit::<ffi::zlink_msg_t>::uninit();
        unsafe {
            if ffi::zlink_msg_init(attempt.as_mut_ptr()) != 0 {
                let errno = ffi::zlink_errno();
                ffi::zlink_multipart_close(native_parts.as_mut_ptr(), native_parts.len());
                return Err(SubmitError::new(SubmitResult::InternalError, errno));
            }
            if ffi::zlink_msg_copy(attempt.as_mut_ptr(), part) != 0 {
                let errno = ffi::zlink_errno();
                ffi::zlink_msg_close(attempt.as_mut_ptr());
                ffi::zlink_multipart_close(native_parts.as_mut_ptr(), native_parts.len());
                return Err(SubmitError::new(
                    SubmitResult::InternalError,
                    if errno == 0 { libc::EIO } else { errno },
                ));
            }
            native_parts.push(attempt.assume_init());
        }
    }

    let rc = submit(native_parts.as_mut_ptr(), native_parts.len());
    let errno = if rc == 0 {
        0
    } else {
        unsafe { ffi::zlink_errno() }
    };
    unsafe {
        ffi::zlink_multipart_close(native_parts.as_mut_ptr(), native_parts.len());
    }
    Ok((rc, errno))
}

/// Transfers builder-owned message parts to a synchronous Core terminal.
/// Core consumes every input slot for every result.
pub(super) fn submit_owned_message(
    parts: &mut MessageParts,
    submit: impl FnOnce(*mut ffi::zlink_msg_t, usize) -> i32,
) -> Result<(i32, i32), SubmitError> {
    if parts.is_empty() {
        return Err(SubmitError::new(
            SubmitResult::InvalidArgument,
            libc::EINVAL,
        ));
    }

    let count = parts.len();
    let rc = submit(parts.as_mut_ptr(), count);
    let errno = if rc == 0 {
        0
    } else {
        unsafe { ffi::zlink_errno() }
    };
    parts.close_parts();
    Ok((rc, errno))
}

pub(super) fn check_submit_result(rc: i32, errno: i32) -> Result<(), SubmitError> {
    if rc == SubmitResult::Ok as i32 {
        Ok(())
    } else {
        Err(submit_error_from_rc(rc, errno))
    }
}

/// A WRITABLE wait exists only for BACKPRESSURED with EAGAIN and a nonzero
/// wait token (bindings spec "Submit 결과 투영"). Any other combination that
/// carries a token is a Core protocol failure, not a wait.
pub(super) fn is_writable_wait(rc: i32, errno: i32, completion_id: u64) -> bool {
    rc == SubmitResult::Backpressured as i32 && errno == libc::EAGAIN && completion_id != 0
}

fn submit_send_attempt(
    op: &mut SendOpStorage,
    user_context: *mut c_void,
) -> Result<SendAttempt, SendAttemptError> {
    let target: *const ffi::zlink_routing_id_t = op
        .target
        .as_ref()
        .map_or(std::ptr::null(), |rid| rid.as_raw() as *const _);
    let mut completion_id = 0;
    let handle = live_handle(op).map_err(SendAttemptError::without_token)?;
    let (rc, errno) = submit_shared_message(&mut op.parts, |parts, count| unsafe {
        if target.is_null() {
            ffi::zlink_send(
                handle,
                parts,
                count,
                ffi::ZLINK_DONTWAIT,
                user_context,
                &mut completion_id,
            )
        } else {
            ffi::zlink_send_rid(
                handle,
                target,
                parts,
                count,
                ffi::ZLINK_DONTWAIT,
                user_context,
                &mut completion_id,
            )
        }
    })
    .map_err(SendAttemptError::without_token)?;
    if rc == 0 {
        if completion_id != 0 {
            return Err(SendAttemptError {
                error: SubmitError::new(SubmitResult::InternalError, libc::EPROTO),
                live_token: Some(completion_id),
            });
        }
        return Ok(SendAttempt::Admitted);
    }
    if is_writable_wait(rc, errno, completion_id) {
        return Ok(SendAttempt::Waiting(completion_id));
    }
    if completion_id != 0 || rc == SubmitResult::Backpressured as i32 {
        return Err(SendAttemptError {
            error: SubmitError::new(SubmitResult::InternalError, libc::EPROTO),
            live_token: (completion_id != 0).then_some(completion_id),
        });
    }
    Err(SendAttemptError::without_token(submit_error_from_rc(
        rc, errno,
    )))
}

fn live_handle(op: &SendOpStorage) -> Result<*mut c_void, SubmitError> {
    let handle = op
        .routed
        .as_ref()
        .map_or(op.handle, |routed| routed.handle());
    if handle.is_null() {
        Err(SubmitError::new(
            SubmitResult::InternalError,
            libc::ECANCELED,
        ))
    } else {
        Ok(handle)
    }
}

#[cfg(test)]
mod writable_wait_tests {
    use super::*;

    #[test]
    fn writable_wait_requires_backpressure_eagain_and_nonzero_token() {
        let backpressured = SubmitResult::Backpressured as i32;
        assert!(is_writable_wait(backpressured, libc::EAGAIN, 7));
        assert!(!is_writable_wait(backpressured, libc::ETIMEDOUT, 7));
        assert!(!is_writable_wait(backpressured, libc::ENOBUFS, 7));
        assert!(!is_writable_wait(backpressured, 0, 7));
        assert!(!is_writable_wait(backpressured, libc::EAGAIN, 0));
        assert!(!is_writable_wait(SubmitResult::Ok as i32, libc::EAGAIN, 7));
        assert!(!is_writable_wait(
            SubmitResult::NotConnected as i32,
            libc::EAGAIN,
            7
        ));
    }
}
