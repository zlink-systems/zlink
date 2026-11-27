// SPDX-License-Identifier: MPL-2.0

use crate::{
    Empty, Message, PublishOp, Ready, ReplyOp, RequestOp, RequestSubmission, SendFlags, SendOp,
    SendSubmission, SubmitError, ZlinkError,
};
use std::marker::PhantomData;
use std::time::Duration;

macro_rules! first_part {
    ($name:ident) => {
        impl $name<Empty> {
            #[inline]
            pub fn message(self, message: Message) -> $name<Ready> {
                let $name { mut inner, .. } = self;
                inner.parts.push(message);
                $name {
                    inner,
                    _state: PhantomData,
                }
            }
        }
    };
}

macro_rules! more_parts {
    ($name:ident) => {
        impl $name<Ready> {
            #[inline]
            pub fn message(mut self, message: Message) -> Self {
                self.inner.parts.push(message);
                self
            }
        }
    };
}

first_part!(SendOp);
more_parts!(SendOp);
first_part!(PublishOp);
more_parts!(PublishOp);
first_part!(RequestOp);
more_parts!(RequestOp);
first_part!(ReplyOp);
more_parts!(ReplyOp);

impl SendOp<Ready> {
    /// Starts a nonblocking admission attempt and returns its result.
    ///
    /// An immediately admitted SEND has completion ID zero and resolves on the
    /// first poll. Under backpressure the future retains the packet, waits for
    /// its exact WRITABLE token through the socket poller, and retries the same
    /// packet. No ordinary SEND completion is produced. If backpressure needs
    /// a completion and no public `POLLCOMPLETION` poller owns the socket, this
    /// returns `SubmitResult::InvalidState`.
    pub fn submit(self) -> Result<SendSubmission, SubmitError> {
        self.inner.submit_send()
    }

    /// Uses Core blocking admission (`NONE`).
    pub fn submit_sync(self) -> Result<(), SubmitError> {
        self.inner.submit_send_blocking()
    }
}

impl PublishOp<Ready> {
    pub fn flags(mut self, flags: SendFlags) -> Self {
        self.inner.flags = flags;
        self
    }
    pub fn submit(self) -> Result<(), SubmitError> {
        self.inner.submit_publish()
    }
}

impl RequestOp<Ready> {
    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.inner.timeout = timeout;
        self
    }

    /// Starts nonblocking REQUEST admission and returns its admission and reply
    /// stages.
    ///
    /// Under backpressure the future retains the multipart request, waits for
    /// its exact WRITABLE token, and resubmits it once. Repeated refusals repeat
    /// that transition. The reply timeout starts only after admission. A public
    /// `POLLCOMPLETION` poller must own the socket before submission; otherwise
    /// this returns `SubmitResult::InvalidState`.
    pub fn submit(self) -> Result<RequestSubmission, ZlinkError> {
        self.inner.submit_routed_request()
    }

    /// Uses Core blocking admission and waits for the reply. Without a public
    /// completion owner, the calling thread drains this request in-line.
    pub fn submit_sync(self) -> Result<Vec<Message>, ZlinkError> {
        self.inner.submit_routed_request_sync()
    }
}

impl ReplyOp<Ready> {
    pub fn submit(self) -> Result<(), SubmitError> {
        self.inner.submit_reply()
    }
}
