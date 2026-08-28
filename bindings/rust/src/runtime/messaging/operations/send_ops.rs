// SPDX-License-Identifier: MPL-2.0

//! Send and publish terminals for the Core 0.14.0 contract.
//!
//! HWM-managed send (PAIR send, DEALER/ROUTER routed send, STREAM peer send,
//! and the routed send addressed at a received envelope) has an asynchronous
//! `zlink_send_async` terminal and a synchronous `zlink_send_part(_rid)`
//! terminal with send flags. The binding owns no thread, park queue, retry or
//! deadline timer.
//!
//! Publish is synchronous: PUB semantics are lossy, the publisher never waits
//! at a HWM, and `zlink_send_async` answers `ENOTSUP` for PUB/XPUB.

use std::ffi::c_void;
use std::future::Future;
use std::mem::MaybeUninit;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};
use std::time::Duration;

use crate::error::{SubmitError, SubmitResult};
use crate::ffi;
use crate::internal::{RoutedHandle, SendCompletionSlot, SendCompletions};
use crate::message::Message;
use crate::messaging_operations::{
    Empty, MessageParts, PublishOp, PublishOpStorage, RoutedSendOp, RoutedSendOpStorage, SendOp,
    SendOpKind, SendOpStorage,
};
use crate::native_errors::{check_submit_rc, submit_error_from_errno, submit_validation_error};
use crate::socket::submit_part_sequence;

// -- Builder factories -------------------------------------------------------

pub(crate) fn socket_send_op(
    handle: *mut c_void,
    completions: Option<Arc<SendCompletions>>,
) -> SendOp<Empty> {
    SendOp {
        inner: SendOpStorage {
            handle,
            routed: None,
            completions,
            kind: SendOpKind::Plain,
            target: None,
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn socket_routed_send_op(
    routed: Arc<RoutedHandle>,
    completions: Arc<SendCompletions>,
    target: crate::message::RoutingId,
) -> RoutedSendOp<Empty> {
    RoutedSendOp {
        inner: RoutedSendOpStorage {
            routed,
            completions,
            target: Some(target),
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn dealer_routed_send_op(
    routed: Arc<RoutedHandle>,
    completions: Arc<SendCompletions>,
) -> RoutedSendOp<Empty> {
    RoutedSendOp {
        inner: RoutedSendOpStorage {
            routed,
            completions,
            target: None,
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn stream_send_to_op(
    handle: *mut c_void,
    completions: Option<Arc<SendCompletions>>,
    target: crate::message::RoutingId,
) -> SendOp<Empty> {
    SendOp {
        inner: SendOpStorage {
            handle,
            routed: None,
            completions,
            kind: SendOpKind::StreamRouted,
            target: Some(target),
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn received_routed_send_op(
    routed: Arc<RoutedHandle>,
    completions: Arc<SendCompletions>,
    target: crate::message::RoutingId,
) -> SendOp<Empty> {
    SendOp {
        inner: SendOpStorage {
            handle: routed.handle(),
            routed: Some(routed),
            completions: Some(completions),
            kind: SendOpKind::Routed,
            target: Some(target),
            parts: MessageParts::default(),
            timeout: Duration::ZERO,
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
            flags: crate::flags::SendFlags::NONE,
        },
        _state: std::marker::PhantomData,
    }
}

// -- Publish (synchronous) ---------------------------------------------------

pub(crate) fn submit_publish(mut op: PublishOpStorage) -> Result<(), SubmitError> {
    let flags = op.flags.bits();
    let mut topic_buf = [0u8; 256];
    let topic_bytes = op.topic.as_str().as_bytes();
    topic_buf[..topic_bytes.len()].copy_from_slice(topic_bytes);
    let handle = op.handle;
    let rc = submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
        ffi::zlink_publish_part(handle, topic_buf.as_ptr().cast(), part, flags, part_flag)
    })?;
    drop(op.parts);
    check_submit_rc(rc)
}

// -- HWM-managed send (asynchronous) -----------------------------------------

pub(crate) fn submit_send(op: SendOpStorage) -> impl Future<Output = Result<(), SubmitError>> + Send {
    SendFuture::new(op)
}

pub(crate) fn submit_routed_send(
    op: RoutedSendOpStorage,
) -> impl Future<Output = Result<(), SubmitError>> + Send {
    SendFuture::new(SendOpStorage {
        handle: op.routed.handle(),
        routed: Some(op.routed),
        completions: Some(op.completions),
        kind: SendOpKind::Routed,
        target: op.target,
        parts: op.parts,
        timeout: op.timeout,
    })
}

// -- HWM-managed send (synchronous + flags) ---------------------------------

pub(crate) fn submit_send_blocking(
    mut op: SendOpStorage,
    flags: crate::flags::SendFlags,
) -> Result<(), SubmitError> {
    let handle = op
        .routed
        .as_ref()
        .map_or(op.handle, |routed| routed.handle());
    if handle.is_null() {
        return Err(submit_error_from_errno(libc::ECANCELED));
    }

    let target = op.target.as_ref().map(|rid| rid.as_raw() as *const _);
    let rc = submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
        match target {
            Some(target) => {
                ffi::zlink_send_part_rid(handle, target, part, flags.bits(), part_flag)
            }
            None => ffi::zlink_send_part(handle, part, flags.bits(), part_flag),
        }
    })?;
    drop(op.parts);
    check_submit_rc(rc)
}

pub(crate) fn submit_routed_send_blocking(
    op: RoutedSendOpStorage,
    flags: crate::flags::SendFlags,
) -> Result<(), SubmitError> {
    submit_send_blocking(
        SendOpStorage {
            handle: op.routed.handle(),
            routed: Some(op.routed),
            completions: Some(op.completions),
            kind: SendOpKind::Routed,
            target: op.target,
            parts: op.parts,
            timeout: op.timeout,
        },
        flags,
    )
}

/// Consumer side of one Core send operation.
///
/// The Future is inert until first polled. On that poll it performs exactly one
/// `zlink_send_async`; when Core admits the record immediately the completion
/// runs inline and the same poll returns `Ready`. Otherwise Core owns the wait
/// and the completion callback wakes this Future in whatever context Core
/// delivered the completion on.
struct SendFuture {
    operation: Option<SendOpStorage>,
    slot: Option<Arc<SendCompletionSlot>>,
    handle: *mut c_void,
    finished: bool,
}

// The native handle is only dereferenced by Core calls; the storage itself is
// move-safe across threads, exactly like `SendOpStorage`.
unsafe impl Send for SendFuture {}

impl SendFuture {
    fn new(operation: SendOpStorage) -> Self {
        Self {
            handle: operation.handle,
            operation: Some(operation),
            slot: None,
            finished: false,
        }
    }
}

impl Future for SendFuture {
    type Output = Result<(), SubmitError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.finished {
            panic!("send Future polled after completion");
        }
        if let Some(slot) = self.slot.clone() {
            let outcome = slot.poll(cx.waker());
            if outcome.is_ready() {
                self.finished = true;
            }
            return outcome;
        }

        let operation = self.operation.take().expect("active send");
        match start_send(operation) {
            Err(error) => {
                self.finished = true;
                Poll::Ready(Err(error))
            }
            Ok(StartedSend::Completed) => {
                self.finished = true;
                Poll::Ready(Ok(()))
            }
            Ok(StartedSend::Pending { slot, handle }) => {
                self.handle = handle;
                let outcome = slot.poll(cx.waker());
                if outcome.is_ready() {
                    self.finished = true;
                    return outcome;
                }
                self.slot = Some(slot);
                Poll::Pending
            }
        }
    }
}

impl Drop for SendFuture {
    fn drop(&mut self) {
        if self.finished {
            return;
        }
        let Some(slot) = self.slot.take() else {
            return;
        };
        // Ask Core to cancel. The operation still completes exactly once; the
        // socket-scoped registry keeps the slot alive until it does, and the
        // op id makes the cancel ABA-safe.
        let op_id = slot.op_id();
        slot.detach();
        if op_id != 0 && !self.handle.is_null() {
            unsafe {
                ffi::zlink_send_async_cancel(self.handle, op_id);
            }
        }
    }
}

enum StartedSend {
    /// A synchronous lane finished inside the first poll.
    Completed,
    Pending {
        slot: Arc<SendCompletionSlot>,
        handle: *mut c_void,
    },
}

fn start_send(op: SendOpStorage) -> Result<StartedSend, SubmitError> {
    if op.parts.is_empty() {
        return Err(submit_validation_error());
    }

    let completions = op
        .completions
        .clone()
        .ok_or_else(|| SubmitError::new(SubmitResult::NotSupported, libc::ENOTSUP))?;

    // Exact routed target. ROUTER must name one; DEALER passes none and lets
    // Core commit one selection at submit time.
    let target = if matches!(op.kind, SendOpKind::StreamRouted) {
        // A RID-only target asks Core to snapshot the current STREAM transport
        // pair as part of this submit. Once accepted, Core owns the part and
        // every HWM retry until the completion callback resolves the Future.
        let peer_rid = op.target.as_ref().expect("STREAM send target");
        Some(ffi::zlink_routed_submit_target_t {
            peer_rid: *peer_rid.as_raw(),
            transport_pair_id: 0,
            transport_pair_generation: 0,
        })
    } else {
        match op.routed.as_ref() {
            // ROUTER names the peer; DEALER passes none and Core commits one
            // weighted selection. Either way the record is pinned to one exact
            // physical pipe, so a multipart record cannot straddle two pipes.
            Some(routed) => {
                let (rc, selected) = routed.select_target(op.target.as_ref());
                if rc != 0 {
                    return Err(submit_error_from_errno(unsafe { ffi::zlink_errno() }));
                }
                Some(selected)
            }
            None => None,
        }
    };
    let target_ptr: *const ffi::zlink_routed_submit_target_t = match target.as_ref() {
        Some(target) => target,
        None => std::ptr::null(),
    };

    let handle = match op.routed.as_ref() {
        Some(routed) => routed.handle(),
        None => op.handle,
    };
    if handle.is_null() {
        return Err(submit_error_from_errno(libc::ECANCELED));
    }

    let timeout_ms = duration_to_timeout_ms(op.timeout);
    let (slot, userdata) = completions.register();
    let options = ffi::zlink_send_async_options_t {
        struct_size: std::mem::size_of::<ffi::zlink_send_async_options_t>() as u32,
        timeout_ms,
        userdata,
        target: target_ptr,
    };

    let mut parts: Vec<Message> = op.parts.into_vec();
    let mut op_id: ffi::zlink_send_op_id_t = 0;
    let rc = with_moved_native_parts(&mut parts, |native, count| unsafe {
        ffi::zlink_send_async(handle, native, count, &options, &mut op_id)
    });

    if rc != SubmitResult::Ok as i32 {
        completions.discard(userdata);
        return Err(check_submit_rc(rc).expect_err("failed send submit"));
    }
    // On ZLINK_SUBMIT_OK Core owns every native part; the moved-from `Message`
    // values left behind are empty frames that close to nothing.
    drop(parts);

    if op_id == 0 {
        // Core completed synchronously and deliberately emits no callback.
        completions.discard(userdata);
        return Ok(StartedSend::Completed);
    }
    slot.arm(op_id);
    Ok(StartedSend::Pending { slot, handle })
}

fn duration_to_timeout_ms(duration: Duration) -> u32 {
    if duration.is_zero() {
        return 0;
    }
    duration.as_millis().clamp(1, u32::MAX as u128) as u32
}

/// Moves every part into one contiguous native array for `zlink_send_async`
/// and moves the parts back when that asynchronous submit returns non-OK.
/// Unlike synchronous part APIs, `zlink_send_async` takes ownership of
/// `parts_[0 .. count)` only on `ZLINK_SUBMIT_OK`.
fn with_moved_native_parts(
    parts: &mut [Message],
    call: impl FnOnce(*mut ffi::zlink_msg_t, usize) -> i32,
) -> i32 {
    let mut native: Vec<ffi::zlink_msg_t> = Vec::with_capacity(parts.len());
    unsafe {
        for part in parts.iter_mut() {
            let mut slot = MaybeUninit::<ffi::zlink_msg_t>::uninit();
            ffi::zlink_msg_init(slot.as_mut_ptr());
            ffi::zlink_msg_move(slot.as_mut_ptr(), part.raw_mut());
            native.push(slot.assume_init());
        }
    }
    let count = native.len();
    let rc = call(native.as_mut_ptr(), count);
    if rc != SubmitResult::Ok as i32 {
        unsafe {
            for (slot, part) in native.iter_mut().zip(parts.iter_mut()) {
                ffi::zlink_msg_move(part.raw_mut(), slot);
                ffi::zlink_msg_close(slot);
            }
        }
    }
    rc
}
