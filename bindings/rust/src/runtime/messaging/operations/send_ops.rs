// SPDX-License-Identifier: MPL-2.0

use std::ffi::c_void;
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};

use crate::error::{SubmitError, SubmitResult};
use crate::ffi;
use crate::internal::{CompletionEntry, CompletionKind, CompletionOwner, RoutedHandle};
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
        finished: false,
    }
}

pub(crate) fn submit_send_blocking(mut op: SendOpStorage) -> Result<(), SubmitError> {
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
}

struct SendFuture {
    operation: Option<SendOpStorage>,
    entry: Option<Arc<CompletionEntry>>,
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
            let operation = self.operation.take().expect("active send");
            match start_send(operation) {
                Ok(entry) => self.entry = Some(entry),
                Err(error) => {
                    self.finished = true;
                    return Poll::Ready(Err(error));
                }
            }
        }
        let outcome = self
            .entry
            .as_ref()
            .expect("send completion")
            .poll_send(cx.waker());
        if outcome.is_ready() {
            self.finished = true;
        }
        outcome
    }
}

impl Drop for SendFuture {
    fn drop(&mut self) {
        if !self.finished {
            if let Some(entry) = &self.entry {
                entry.detach();
            }
        }
    }
}

fn start_send(mut op: SendOpStorage) -> Result<Arc<CompletionEntry>, SubmitError> {
    if op.parts.is_empty() {
        return Err(SubmitError::new(
            SubmitResult::InvalidArgument,
            libc::EINVAL,
        ));
    }
    let handle = live_handle(&op)?;
    let owner = Arc::clone(&op.completion_owner);
    let (entry, user_context) = owner.register(CompletionKind::Send)?;
    let target: *const ffi::zlink_routing_id_t = op
        .target
        .as_ref()
        .map_or(std::ptr::null(), |rid| rid.as_raw() as *const _);
    let mut completion_id = 0;
    let rc = submit_part_sequence(&mut op.parts, |part, part_flag, is_final| unsafe {
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
    if rc != 0 {
        owner.unregister(user_context);
        return Err(check_submit_rc(rc).expect_err("failed send submit"));
    }
    entry.publish(completion_id);
    if completion_id == 0 {
        entry.capture_inline_send();
        owner.unregister(user_context);
    }
    Ok(entry)
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
