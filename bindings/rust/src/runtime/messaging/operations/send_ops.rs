// SPDX-License-Identifier: MPL-2.0

use std::ffi::c_void;

use crate::error::SubmitError;
use crate::ffi;
use crate::messaging_operations::{
    MessageParts, RoutedSendOp, RoutedSendOpStorage, SendOp, SendOpKind, SendOpStorage,
};
use crate::native_errors::check_submit_rc;
use crate::socket::submit_part_sequence;

pub(crate) fn socket_send_op(handle: *mut c_void) -> SendOp<crate::messaging_operations::Empty> {
    SendOp {
        inner: SendOpStorage {
            handle,
            admission: None,
            kind: SendOpKind::Plain,
            target: None,
            parts: MessageParts::default(),
            flags: crate::flags::SendFlags::NONE,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn socket_routed_send_op(
    admission: std::sync::Arc<crate::internal::RoutedAdmission>,
    target: crate::message::RoutingId,
) -> RoutedSendOp<crate::messaging_operations::Empty> {
    RoutedSendOp {
        inner: RoutedSendOpStorage {
            admission,
            target: Some(target),
            parts: MessageParts::default(),
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn stream_send_to_op(
    handle: *mut c_void,
    target: crate::message::RoutingId,
) -> SendOp<crate::messaging_operations::Empty> {
    SendOp {
        inner: SendOpStorage {
            handle,
            admission: None,
            kind: SendOpKind::StreamRouted,
            target: Some(target),
            parts: MessageParts::default(),
            flags: crate::flags::SendFlags::NONE,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn immediate_routed_send_op(
    admission: std::sync::Arc<crate::internal::RoutedAdmission>,
    target: crate::message::RoutingId,
) -> SendOp<crate::messaging_operations::Empty> {
    SendOp {
        inner: SendOpStorage {
            handle: admission.handle(),
            admission: Some(admission),
            kind: SendOpKind::ImmediateRouted,
            target: Some(target),
            parts: MessageParts::default(),
            flags: crate::flags::SendFlags::NONE,
        },
        _state: std::marker::PhantomData,
    }
}

pub(crate) fn dealer_routed_send_op(
    admission: std::sync::Arc<crate::internal::RoutedAdmission>,
) -> RoutedSendOp<crate::messaging_operations::Empty> {
    RoutedSendOp {
        inner: RoutedSendOpStorage {
            admission,
            target: None,
            parts: MessageParts::default(),
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
            admission: None,
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
        SendOpKind::StreamRouted => {
            let target = op.target.as_ref().expect("STREAM send target");
            submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
                ffi::zlink_send_part_rid(op.handle, target.as_raw(), part, flags, part_flag)
            })?
        }
        SendOpKind::ImmediateRouted => {
            let target = op.target.as_ref().expect("routed send target");
            let admission = op.admission.as_ref().expect("routed admission");
            admission
                .with_attempt_gate(|handle| {
                    submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
                        ffi::zlink_send_part_rid(handle, target.as_raw(), part, flags, part_flag)
                    })
                })
                .ok_or_else(|| crate::native_errors::submit_error_from_errno(libc::ECANCELED))??
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
