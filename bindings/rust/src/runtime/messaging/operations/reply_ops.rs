// SPDX-License-Identifier: MPL-2.0

use std::marker::PhantomData;
use std::sync::Arc;

use crate::error::{SubmitError, SubmitResult};
use crate::ffi;
use crate::message::RoutingId;
use crate::messaging_operations::{Empty, MessageParts, ReplyOp, ReplyOpStorage};
use crate::native_errors::submit_error_from_errno;

use super::send_ops::{check_submit_result, submit_shared_message};

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
    let (rc, errno) = submit_shared_message(&mut op.parts, |parts, count| unsafe {
        ffi::zlink_reply(handle, target, value, parts, count)
    })?;
    check_submit_result(rc, errno)
}
