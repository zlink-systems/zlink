// SPDX-License-Identifier: MPL-2.0

use std::ffi::c_void;
use std::marker::PhantomData;

use crate::error::SubmitError;
use crate::ffi;
use crate::flags::SendFlags;
use crate::message::RoutingId;
use crate::messaging_operations::{Empty, MessageParts, ReplyOp, ReplyOpKind, ReplyOpStorage};
use crate::native_errors::{check_submit_rc, submit_not_supported_error};
use crate::socket::submit_part_sequence;

pub(crate) fn router_reply_op(
    handle: *mut c_void,
    rid: RoutingId,
    request_seq: u64,
) -> ReplyOp<Empty> {
    ReplyOp {
        inner: ReplyOpStorage {
            handle,
            kind: ReplyOpKind::RouterReply { rid, request_seq },
            parts: MessageParts::default(),
            flags: SendFlags::NONE,
        },
        _state: PhantomData,
    }
}

pub(crate) fn submit_reply(mut op: ReplyOpStorage) -> Result<(), SubmitError> {
    if op.flags.bits() != 0 {
        return Err(submit_not_supported_error());
    }

    let ReplyOpKind::RouterReply { rid, request_seq } = &op.kind;
    let rid = rid.as_raw() as *const ffi::zlink_routing_id_t;
    let rc = submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
        ffi::zlink_router_reply_part(op.handle, rid, *request_seq, part, part_flag)
    })?;
    drop(op.parts);
    check_submit_rc(rc)
}
