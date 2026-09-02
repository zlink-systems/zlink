// SPDX-License-Identifier: MPL-2.0

use std::marker::PhantomData;
use std::sync::Arc;

use crate::error::{SubmitError, SubmitResult};
use crate::ffi;
use crate::message::RoutingId;
use crate::messaging_operations::{Empty, MessageParts, ReplyOp, ReplyOpStorage};
use crate::native_errors::{check_submit_rc, submit_error_from_errno};
use crate::socket::submit_part_sequence;

pub(crate) fn router_reply_op(
    routed: Arc<crate::internal::RoutedHandle>,
    owner: Arc<crate::internal::RouterOwnerTag>,
    target: RoutingId,
    token: crate::ReplyToken,
) -> ReplyOp<Empty> {
    ReplyOp {
        inner: ReplyOpStorage {
            routed,
            owner,
            target,
            token,
            parts: MessageParts::default(),
        },
        _state: PhantomData,
    }
}

pub(crate) fn submit_reply(mut op: ReplyOpStorage) -> Result<(), SubmitError> {
    if !op.token.owner_matches(&op.owner) {
        return Err(SubmitError::new(
            SubmitResult::InvalidArgument,
            libc::EINVAL,
        ));
    }
    let handle = op.routed.handle();
    if handle.is_null() {
        return Err(submit_error_from_errno(libc::ECANCELED));
    }
    let target = op.target.as_raw() as *const _;
    let value = op.token.value();
    let rc = submit_part_sequence(&mut op.parts, |part, part_flag, _| unsafe {
        ffi::zlink_reply_part(handle, target, value, part, part_flag)
    })?;
    check_submit_rc(rc)
}
