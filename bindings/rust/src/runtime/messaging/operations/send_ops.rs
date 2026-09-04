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
    Empty, MessageParts, PublishOp, PublishOpStorage, SendOp, SendOpStorage,
};
use crate::native_errors::{check_submit_rc, submit_error_from_errno};
use crate::socket::submit_part_sequence;

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
    let rc = submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
        ffi::zlink_publish_part(op.handle, topic_buf.as_ptr().cast(), part, flags, part_flag)
    })?;
    check_submit_rc(rc)
}

pub(crate) fn submit_send(
    op: SendOpStorage,
) -> impl Future<Output = Result<(), SubmitError>> + Send {
    SendFuture {
        operation: Some(op),
        entry: None,
        waiting_for_writable: false,
        finished: false,
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
        let rc = submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
            if target.is_null() {
                ffi::zlink_send_part(
                    handle,
                    part,
                    0,
                    part_flag,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                )
            } else {
                ffi::zlink_send_part_rid(
                    handle,
                    target,
                    part,
                    0,
                    part_flag,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                )
            }
        })?;
        check_submit_rc(rc)
    })
}

struct SendFuture {
    operation: Option<SendOpStorage>,
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
        if self.entry.is_none() {
            let operation = self.operation.as_ref().expect("active send");
            if operation.parts.is_empty() {
                return self.finish(Err(SubmitError::new(
                    SubmitResult::InvalidArgument,
                    libc::EINVAL,
                )));
            }
            if let Err(error) = live_handle(operation) {
                return self.finish(Err(error));
            }
            let owner = Arc::clone(&operation.completion_owner);
            match owner.register_send(operation.target) {
                Ok((entry, _)) => self.entry = Some(entry),
                Err(error) => {
                    return self.finish(Err(error));
                }
            }
        }

        loop {
            let entry = Arc::clone(self.entry.as_ref().expect("send retry entry"));
            let owner = Arc::clone(
                &self
                    .operation
                    .as_ref()
                    .expect("active send")
                    .completion_owner,
            );
            if self.waiting_for_writable {
                match entry.poll_writable(cx.waker()) {
                    Poll::Ready(Ok(())) => self.waiting_for_writable = false,
                    Poll::Ready(Err(error)) => return self.finish(Err(error)),
                    Poll::Pending => {
                        let self_driven = match owner.drive_send_reactor() {
                            Ok(value) => value,
                            Err(error) => {
                                return self.finish_detached(Err(submit_error_from_errno(
                                    error.native_errno(),
                                )));
                            }
                        };
                        if self_driven {
                            // No waitable fd is exposed by Core. Schedule a
                            // nonblocking reactor probe on the next executor
                            // turn; public Poller ownership wakes this entry
                            // directly instead.
                            cx.waker().wake_by_ref();
                        }
                        return Poll::Pending;
                    }
                }
            } else if let Err(error) = owner.drive_send_reactor() {
                return self.finish(Err(submit_error_from_errno(error.native_errno())));
            }

            let user_context = Arc::as_ptr(&entry) as *mut c_void;
            let attempt = {
                let operation = self.operation.as_mut().expect("active send");
                submit_send_attempt(operation, user_context)
            };
            match attempt {
                Ok(SendAttempt::Admitted) => return self.finish(Ok(())),
                Ok(SendAttempt::Waiting(completion_id)) => {
                    entry.publish(completion_id);
                    self.waiting_for_writable = true;
                }
                Err(failure) => {
                    if let Some(completion_id) = failure.live_token {
                        entry.publish(completion_id);
                        return self.finish_detached(Err(failure.error));
                    }
                    return self.finish(Err(failure.error));
                }
            }
        }
    }
}

impl SendFuture {
    fn finish(&mut self, result: Result<(), SubmitError>) -> Poll<Result<(), SubmitError>> {
        if let Some(entry) = self.entry.take() {
            let user_context = Arc::as_ptr(&entry) as *mut c_void;
            if let Some(operation) = &self.operation {
                operation.completion_owner.unregister(user_context);
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
        if let Some(entry) = self.entry.take() {
            let user_context = Arc::as_ptr(&entry) as *mut c_void;
            let captured = entry.detach();
            if captured {
                if let Some(operation) = &self.operation {
                    operation.completion_owner.unregister(user_context);
                }
            }
        }
        self.operation.take();
        self.waiting_for_writable = false;
        self.finished = true;
        Poll::Ready(result)
    }
}

impl Drop for SendFuture {
    fn drop(&mut self) {
        if !self.finished {
            if let Some(entry) = &self.entry {
                let captured = entry.detach();
                if captured {
                    if let Some(operation) = &self.operation {
                        operation
                            .completion_owner
                            .unregister(Arc::as_ptr(entry) as *mut c_void);
                    }
                }
            }
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

fn submit_send_attempt(
    op: &mut SendOpStorage,
    user_context: *mut c_void,
) -> Result<SendAttempt, SendAttemptError> {
    let mut attempt_parts = op.parts.try_clone().map_err(|error| {
        SendAttemptError::without_token(submit_error_from_errno(if error.native_errno() == 0 {
            libc::EIO
        } else {
            error.native_errno()
        }))
    })?;
    let target: *const ffi::zlink_routing_id_t = op
        .target
        .as_ref()
        .map_or(std::ptr::null(), |rid| rid.as_raw() as *const _);
    let mut completion_id = 0;
    let owner = Arc::clone(&op.completion_owner);
    let (rc, errno) = owner
        .with_submit(|| {
            let handle = live_handle(op)?;
            let rc =
                submit_part_sequence(&mut attempt_parts, |part, part_flag, is_final| unsafe {
                    let context = if is_final {
                        user_context
                    } else {
                        std::ptr::null_mut()
                    };
                    let id_out = if is_final {
                        &mut completion_id
                    } else {
                        std::ptr::null_mut()
                    };
                    if target.is_null() {
                        ffi::zlink_send_part(
                            handle,
                            part,
                            ffi::ZLINK_DONTWAIT,
                            part_flag,
                            context,
                            id_out,
                        )
                    } else {
                        ffi::zlink_send_part_rid(
                            handle,
                            target,
                            part,
                            ffi::ZLINK_DONTWAIT,
                            part_flag,
                            context,
                            id_out,
                        )
                    }
                })?;
            Ok((rc, unsafe { ffi::zlink_errno() }))
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
    if rc == SubmitResult::Backpressured as i32 && errno == libc::EAGAIN && completion_id != 0 {
        return Ok(SendAttempt::Waiting(completion_id));
    }
    if completion_id != 0 || errno == libc::EAGAIN {
        return Err(SendAttemptError {
            error: SubmitError::new(SubmitResult::InternalError, libc::EPROTO),
            live_token: (completion_id != 0).then_some(completion_id),
        });
    }
    Err(SendAttemptError::without_token(submit_error_from_errno(
        errno,
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
