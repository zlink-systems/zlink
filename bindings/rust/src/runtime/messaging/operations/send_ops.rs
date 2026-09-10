// SPDX-License-Identifier: MPL-2.0

use std::ffi::c_void;
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};

use crate::error::{SubmitError, SubmitResult};
use crate::ffi;
use crate::internal::{CompletionEntry, CompletionEntryKind, CompletionOwner, RoutedHandle};
use crate::messaging_operations::{
    Empty, MessageParts, PublishOp, PublishOpStorage, SendOp, SendOpStorage, SendSubmission,
};
use crate::native_errors::{submit_error_from_errno, submit_error_from_rc};

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
    topic_buf[..bytes.len()].copy_from_slice(bytes);
    let (rc, errno) = submit_shared_message(&mut op.parts, |parts, count| unsafe {
        ffi::zlink_publish(op.handle, topic_buf.as_ptr().cast(), parts, count, flags)
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

    let context = op.completion_owner.next_context();
    match submit_send_attempt(&mut op, context) {
        Ok(SendAttempt::Admitted) => Ok(SendSubmission {
            result: SubmitResult::Ok,
            admitted: Box::pin(std::future::ready(Ok(()))),
        }),
        Ok(SendAttempt::Waiting(completion_id)) => {
            let entry = CompletionEntry::new(CompletionEntryKind::SendRetry);
            let owner = Arc::clone(&op.completion_owner);
            if let Err(error) = owner.register_send_token(context, &entry, completion_id) {
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
            if let Some(completion_id) = failure.live_token {
                let entry = CompletionEntry::new(CompletionEntryKind::SendRetry);
                let owner = Arc::clone(&op.completion_owner);
                let _ = owner.register_send_token(context, &entry, completion_id);
                if entry.detach() {
                    owner.unregister(context);
                }
            }
            Err(failure.error)
        }
    }
}

pub(crate) fn submit_send_blocking(mut op: SendOpStorage) -> Result<(), SubmitError> {
    let owner = Arc::clone(&op.completion_owner);
    owner.with_submit(|| {
        let handle = live_handle(&op)?;
        let target: *const ffi::zlink_routing_id_t = op
            .target
            .as_ref()
            .map_or(std::ptr::null(), |rid| rid.as_raw() as *const _);
        let (rc, errno) = submit_shared_message(&mut op.parts, |parts, count| unsafe {
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
    })
}

/// Managed DONTWAIT SEND.
///
/// The packet stays owned here. Each attempt submits Core shared copies of the
/// parts; when Core answers with a wait token the entry is registered with the
/// socket's completion owner and the Future parks until the WRITABLE record
/// for exactly that token (same context, same routed target) is drained by the
/// owner's reactor thread or by the public poller that owns the queue.
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

            let context = self.context;
            let attempt = {
                let operation = self.operation.as_mut().expect("active send");
                submit_send_attempt(operation, context)
            };
            match attempt {
                Ok(SendAttempt::Admitted) => return self.finish(Ok(())),
                Ok(SendAttempt::Waiting(completion_id)) => {
                    if let Err(error) = self.arm(completion_id) {
                        return self.finish_detached(Err(error));
                    }
                    self.waiting_for_writable = true;
                }
                Err(failure) => {
                    if let Some(completion_id) = failure.live_token {
                        let _ = self.arm(completion_id);
                        return self.finish_detached(Err(failure.error));
                    }
                    return self.finish(Err(failure.error));
                }
            }
        }
    }
}

impl SendFuture {
    /// Registers (once) and publishes the wait token Core just issued.
    fn arm(&mut self, completion_id: u64) -> Result<(), SubmitError> {
        let operation = self.operation.as_ref().expect("active send");
        let owner = Arc::clone(&operation.completion_owner);
        let entry = Arc::clone(
            self.entry
                .get_or_insert_with(|| CompletionEntry::new(CompletionEntryKind::SendRetry)),
        );
        owner.register_send_token(self.context, &entry, completion_id)
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
                return Err(submit_error_from_errno(errno));
            }
            if ffi::zlink_msg_copy(attempt.as_mut_ptr(), part.raw_mut()) != 0 {
                let errno = ffi::zlink_errno();
                ffi::zlink_msg_close(attempt.as_mut_ptr());
                ffi::zlink_multipart_close(native_parts.as_mut_ptr(), native_parts.len());
                return Err(submit_error_from_errno(if errno == 0 {
                    libc::EIO
                } else {
                    errno
                }));
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

pub(super) fn check_submit_result(rc: i32, errno: i32) -> Result<(), SubmitError> {
    if rc == SubmitResult::Ok as i32 {
        Ok(())
    } else {
        Err(submit_error_from_rc(rc, errno))
    }
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
    let owner = Arc::clone(&op.completion_owner);
    let (rc, errno) = owner
        .with_submit(|| {
            let handle = live_handle(op)?;
            submit_shared_message(&mut op.parts, |parts, count| unsafe {
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
    if rc == SubmitResult::Backpressured as i32 && completion_id != 0 {
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
        Err(submit_error_from_errno(libc::ECANCELED))
    } else {
        Ok(handle)
    }
}
