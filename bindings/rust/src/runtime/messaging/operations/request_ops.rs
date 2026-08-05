// SPDX-License-Identifier: MPL-2.0

use std::ffi::c_void;
use std::marker::PhantomData;
use std::time::Duration;

use crate::error::{RequestError, SubmitError};
use crate::ffi;
use crate::flags::SendFlags;
use crate::message::RoutingId;
use crate::messaging_operations::{
    Empty, MessageParts, RequestOp, RequestOpKind, RequestOpStorage,
};
use crate::native_errors::{check_submit_rc, submit_validation_error};
use crate::socket::submit_part_sequence;

pub(crate) fn dealer_request_op(handle: *mut c_void) -> RequestOp<Empty> {
    RequestOp {
        inner: RequestOpStorage {
            handle,
            kind: RequestOpKind::DealerRequest,
            peer_rid: None,
            parts: MessageParts::default(),
            flags: None,
            timeout: Duration::ZERO,
        },
        _state: PhantomData,
    }
}

pub(crate) fn router_request_op(handle: *mut c_void, peer_rid: RoutingId) -> RequestOp<Empty> {
    RequestOp {
        inner: RequestOpStorage {
            handle,
            kind: RequestOpKind::RouterRequest,
            peer_rid: Some(peer_rid),
            parts: MessageParts::default(),
            flags: None,
            timeout: Duration::ZERO,
        },
        _state: PhantomData,
    }
}

pub(crate) fn submit_request<F>(
    op: RequestOpStorage,
    flags: SendFlags,
    callback: F,
) -> Result<(), SubmitError>
where
    F: FnOnce(Result<Vec<crate::message::Message>, RequestError>) + Send + 'static,
{
    submit_request_inner(op, flags, callback)
}

fn submit_request_inner<F>(
    mut op: RequestOpStorage,
    flags: SendFlags,
    callback: F,
) -> Result<(), SubmitError>
where
    F: FnOnce(Result<Vec<crate::message::Message>, RequestError>) + Send + 'static,
{
    if op.parts.is_empty() {
        return Err(submit_validation_error());
    }

    let state_ptr = Box::into_raw(Box::new(crate::operations::ReplyCallbackState {
        callback: Some(Box::new(callback)),
    }));
    let timeout_ms = timeout_to_timeout_ms(op.timeout);
    let rc = match &op.kind {
        RequestOpKind::DealerRequest => {
            submit_part_sequence(&mut op.parts, |part, part_flag, is_final| unsafe {
                ffi::zlink_dealer_request_part(
                    op.handle,
                    part,
                    flags.bits(),
                    part_flag,
                    if is_final { timeout_ms } else { 0 },
                    if is_final {
                        Some(crate::operations::reply_callback)
                    } else {
                        None
                    },
                    if is_final {
                        state_ptr.cast()
                    } else {
                        std::ptr::null_mut()
                    },
                )
            })?
        }
        RequestOpKind::RouterRequest => {
            let peer_rid = op
                .peer_rid
                .as_ref()
                .expect("router request operation must have a peer routing id");
            let peer_rid = peer_rid.as_raw() as *const ffi::zlink_routing_id_t;
            submit_part_sequence(&mut op.parts, |part, part_flag, is_final| unsafe {
                ffi::zlink_router_request_part(
                    op.handle,
                    peer_rid,
                    part,
                    flags.bits(),
                    part_flag,
                    if is_final { timeout_ms } else { 0 },
                    if is_final {
                        Some(crate::operations::reply_callback)
                    } else {
                        None
                    },
                    if is_final {
                        state_ptr.cast()
                    } else {
                        std::ptr::null_mut()
                    },
                )
            })?
        }
    };

    if rc != 0 {
        unsafe {
            drop(Box::from_raw(state_ptr));
        }
    }
    check_submit_rc(rc)
}

fn timeout_to_timeout_ms(timeout: Duration) -> u32 {
    timeout
        .as_millis()
        .min(u32::MAX as u128)
        .try_into()
        .unwrap_or(u32::MAX)
}
