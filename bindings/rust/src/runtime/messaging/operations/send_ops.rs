// SPDX-License-Identifier: MPL-2.0

use std::ffi::c_void;

use crate::error::SubmitError;
use crate::ffi;
use crate::messaging_operations::{MessageParts, SendOp, SendOpKind, SendOpStorage};
use crate::native_errors::check_submit_rc;
use crate::socket::submit_part_sequence;

pub(crate) fn socket_send_op(handle: *mut c_void) -> SendOp<crate::messaging_operations::Empty> {
    SendOp {
        inner: SendOpStorage {
            handle,
            kind: SendOpKind::Plain,
            target: None,
            parts: MessageParts::default(),
            flags: crate::flags::SendFlags::NONE,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn socket_send_to_op(
    handle: *mut c_void,
    target: crate::message::RoutingId,
) -> SendOp<crate::messaging_operations::Empty> {
    SendOp {
        inner: SendOpStorage {
            handle,
            kind: SendOpKind::Routed,
            target: Some(target),
            parts: MessageParts::default(),
            flags: crate::flags::SendFlags::NONE,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn socket_publish_op(
    handle: *mut c_void,
    topic: smol_str::SmolStr,
) -> SendOp<crate::messaging_operations::Empty> {
    SendOp {
        inner: SendOpStorage {
            handle,
            kind: SendOpKind::Published { topic },
            target: None,
            parts: MessageParts::default(),
            flags: crate::flags::SendFlags::NONE,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn submit_send(mut op: SendOpStorage) -> Result<bool, SubmitError> {
    let flags = op.flags.bits();
    let rc = match &op.kind {
        SendOpKind::Plain => submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
            ffi::zlink_send_part(op.handle, part, flags, part_flag)
        })?,
        SendOpKind::Routed => {
            let target = op
                .target
                .as_ref()
                .expect("routed send operation must have a target");
            let target = target.as_raw() as *const ffi::zlink_routing_id_t;
            submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
                ffi::zlink_send_part_rid(op.handle, target, part, flags, part_flag)
            })?
        }
        SendOpKind::Published { topic } => {
            let mut topic_buf = [0u8; 256];
            let topic_bytes = topic.as_str().as_bytes();
            topic_buf[..topic_bytes.len()].copy_from_slice(topic_bytes);
            submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
                ffi::zlink_publish_part(
                    op.handle,
                    topic_buf.as_ptr().cast(),
                    part,
                    flags,
                    part_flag,
                )
            })?
        }
    };

    drop(op.parts);
    match check_submit_rc(rc) {
        Ok(()) => Ok(true),
        Err(error) if error.code() == crate::error::SubmitResult::Backpressured => Ok(false),
        Err(error) => Err(error),
    }
}
