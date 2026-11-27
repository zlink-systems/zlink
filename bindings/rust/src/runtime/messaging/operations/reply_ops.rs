// SPDX-License-Identifier: MPL-2.0

use std::marker::PhantomData;
use std::sync::Arc;

use crate::contracts::errors::errors::{SubmitError, SubmitResult};
use crate::contracts::messaging::message::RoutingId;
use crate::contracts::messaging::operations::{Empty, ReplyOp, ReplyOpStorage};
use crate::internal::MessageParts;
use crate::runtime::native::ffi;

use super::send_ops::{check_submit_result, submit_owned_message};

impl ReplyOpStorage {
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
}

impl ReplyOpStorage {
    pub(crate) fn submit_reply(self) -> Result<(), SubmitError> {
        let mut op = self;
        if !op.token.owner_matches(&op.owner) {
            return Err(SubmitError::new(
                SubmitResult::InvalidArgument,
                libc::EINVAL,
            ));
        }
        let handle = op.routed.handle();
        if handle.is_null() {
            return Err(SubmitError::new(
                SubmitResult::InternalError,
                libc::ECANCELED,
            ));
        }
        let target = op.target.as_raw() as *const _;
        let value = op.token.value();
        let (rc, errno) = submit_owned_message(&mut op.parts, |parts, count| unsafe {
            ffi::zlink_reply(handle, target, value, parts, count)
        })?;
        check_submit_result(rc, errno)
    }
}
