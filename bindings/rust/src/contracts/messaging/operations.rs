use std::ffi::c_void;
use std::marker::PhantomData;
use std::sync::Arc;
use std::time::Duration;

use crate::error::{SubmitError, ZlinkError};
use crate::flags::SendFlags;
use crate::message::{Message, RoutingId};

/// Typestate marker: no message has been set yet.
pub struct Empty;

/// Typestate marker: at least one message part has been set.
pub struct Ready;

#[derive(Default)]
pub(crate) struct MessageParts {
    first: Option<Message>,
    rest: Vec<Message>,
}

impl MessageParts {
    pub(crate) fn push(&mut self, message: Message) {
        if self.first.is_none() {
            self.first = Some(message);
        } else {
            self.rest.push(message);
        }
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.first.is_none()
    }
    pub(crate) fn len(&self) -> usize {
        usize::from(self.first.is_some()) + self.rest.len()
    }
    pub(crate) fn iter_mut(&mut self) -> impl Iterator<Item = &mut Message> {
        self.first.iter_mut().chain(self.rest.iter_mut())
    }
}

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
    /// Starts nonblocking admission attempts and resolves when Core admits the
    /// packet.
    ///
    /// An immediately admitted SEND has completion ID zero and resolves on the
    /// first poll. Under backpressure the future retains the packet, waits for
    /// its exact WRITABLE token through the socket poller, and retries the same
    /// packet. No ordinary SEND completion is produced.
    pub fn submit(self) -> impl std::future::Future<Output = Result<(), SubmitError>> + Send {
        crate::operations::submit_send(self.inner)
    }

    /// Uses Core blocking admission (`NONE`).
    pub fn submit_sync(self) -> Result<(), SubmitError> {
        crate::operations::submit_send_blocking(self.inner)
    }
}

impl PublishOp<Ready> {
    pub fn flags(mut self, flags: SendFlags) -> Self {
        self.inner.flags = flags;
        self
    }
    pub fn submit(self) -> Result<(), SubmitError> {
        crate::operations::submit_publish(self.inner)
    }
}

impl RequestOp<Ready> {
    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.inner.timeout = timeout;
        self
    }

    pub fn submit(
        self,
    ) -> impl std::future::Future<Output = Result<Vec<Message>, ZlinkError>> + Send {
        crate::operations::submit_routed_request(self.inner)
    }

    pub fn submit_sync(self) -> Result<Vec<Message>, ZlinkError> {
        crate::operations::submit_routed_request_sync(self.inner)
    }
}

impl ReplyOp<Ready> {
    pub fn submit(self) -> Result<(), SubmitError> {
        crate::operations::submit_reply(self.inner)
    }
}
