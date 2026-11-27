use std::ffi::c_void;
use std::future::Future;
use std::marker::PhantomData;
use std::pin::Pin;
use std::sync::Arc;
use std::time::Duration;

use crate::error::{SubmitError, SubmitResult, ZlinkError};
use crate::flags::SendFlags;
use crate::internal::MessageParts;
use crate::message::{Message, RoutingId};

/// Typestate marker: no message has been set yet.
pub struct Empty;

/// Typestate marker: at least one message part has been set.
pub struct Ready;

/// A multipart HWM-managed send builder.
pub struct SendOp<State> {
    pub(crate) inner: SendOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct SendOpStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) routed: Option<Arc<crate::internal::RoutedHandle>>,
    pub(crate) completion_owner: Arc<crate::internal::CompletionOwner>,
    pub(crate) target: Option<RoutingId>,
    pub(crate) parts: MessageParts,
}

unsafe impl Send for SendOpStorage {}

/// A PUB/XPUB publish builder. Publish has a synchronous terminal.
pub struct PublishOp<State> {
    pub(crate) inner: PublishOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct PublishOpStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) topic: smol_str::SmolStr,
    pub(crate) parts: MessageParts,
    pub(crate) flags: SendFlags,
}

unsafe impl Send for PublishOpStorage {}

/// A completion-backed request builder.
pub struct RequestOp<State> {
    pub(crate) inner: RequestOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct RequestOpStorage {
    pub(crate) routed: Arc<crate::internal::RoutedHandle>,
    pub(crate) completion_owner: Arc<crate::internal::CompletionOwner>,
    pub(crate) peer_rid: Option<RoutingId>,
    pub(crate) parts: MessageParts,
    pub(crate) timeout: Duration,
}

unsafe impl Send for RequestOpStorage {}

/// The result of starting an asynchronous SEND.
pub struct SendSubmission {
    /// Core's admission result from the initial submit attempt.
    pub result: SubmitResult,
    /// Completes when a backpressured packet is eventually admitted.
    pub admitted: Pin<Box<dyn Future<Output = Result<(), SubmitError>> + Send>>,
}

/// The result of starting an asynchronous REQUEST.
pub struct RequestSubmission {
    /// Core's admission result from the initial submit attempt.
    pub result: SubmitResult,
    /// Completes when a backpressured request is eventually admitted.
    pub admitted: Pin<Box<dyn Future<Output = Result<(), SubmitError>> + Send>>,
    /// Completes with the request reply after admission succeeds.
    pub reply: Pin<Box<dyn Future<Output = Result<Vec<Message>, ZlinkError>> + Send>>,
}

/// A ROUTER reply builder.
pub struct ReplyOp<State> {
    pub(crate) inner: ReplyOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct ReplyOpStorage {
    pub(crate) routed: Arc<crate::internal::RoutedHandle>,
    pub(crate) owner: Arc<crate::internal::RouterOwnerTag>,
    pub(crate) target: RoutingId,
    pub(crate) token: crate::ReplyToken,
    pub(crate) parts: MessageParts,
}

unsafe impl Send for ReplyOpStorage {}

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
