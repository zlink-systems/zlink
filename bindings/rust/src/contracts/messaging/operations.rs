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

/// Owns operation parts without allocating for the common single-part case.
/// Multipart operations allocate only when a second part is added.
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

    pub(crate) fn iter(&self) -> impl Iterator<Item = &Message> {
        self.first.iter().chain(self.rest.iter())
    }
}

/// A multipart send builder: add parts with [`message`](SendOp::message), then
/// [`submit`](SendOp::submit).
///
/// Submitting consumes the added [`Message`] parts: on a successful submit each
/// part's payload is moved into the transport and the managed value is left
/// empty, so a part must not be reused after a successful submit. The request
/// and reply builders in this module share this same consume-on-submit
/// ownership model.
///
/// The `State` type parameter is a typestate ([`Empty`] or [`Ready`]) that
/// statically tracks whether at least one part has been added.
pub struct SendOp<State> {
    pub(crate) inner: SendOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct SendOpStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) admission: Option<Arc<crate::internal::RoutedAdmission>>,
    pub(crate) kind: SendOpKind,
    pub(crate) target: Option<RoutingId>,
    pub(crate) parts: MessageParts,
    pub(crate) flags: SendFlags,
}

pub(crate) enum SendOpKind {
    Plain,
    Published { topic: smol_str::SmolStr },
    StreamRouted,
    ImmediateRouted,
}

unsafe impl Send for SendOpStorage {}

/// An HWM-managed routed send builder. Its [`submit`](RoutedSendOp::submit)
/// terminal returns a runtime-independent [`Future`](std::future::Future).
pub struct RoutedSendOp<State> {
    pub(crate) inner: RoutedSendOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct RoutedSendOpStorage {
    pub(crate) admission: Arc<crate::internal::RoutedAdmission>,
    pub(crate) target: Option<RoutingId>,
    pub(crate) parts: MessageParts,
}

/// A request builder: add parts, then submit and await a reply. Parts are
/// consumed on a successful submit (see [`SendOp`]).
pub struct RequestOp<State> {
    pub(crate) inner: RequestOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct RequestOpStorage {
    pub(crate) admission: Arc<crate::internal::RoutedAdmission>,
    pub(crate) peer_rid: Option<RoutingId>,
    pub(crate) parts: MessageParts,
    pub(crate) timeout: Duration,
}

unsafe impl Send for RequestOpStorage {}

/// A reply builder: add parts, then submit. Parts are consumed on a successful
/// submit (see [`SendOp`]).
pub struct ReplyOp<State> {
    pub(crate) inner: ReplyOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct ReplyOpStorage {
    pub(crate) admission: Arc<crate::internal::RoutedAdmission>,
    pub(crate) kind: ReplyOpKind,
    pub(crate) parts: MessageParts,
    pub(crate) flags: SendFlags,
}

pub(crate) enum ReplyOpKind {
    RouterReply { rid: RoutingId, request_seq: u64 },
}

unsafe impl Send for ReplyOpStorage {}

impl SendOp<Empty> {
    /// Adds the first message part, transitioning the builder to the ready
    /// state. The part is consumed on a successful submit (see [`SendOp`]).
    pub fn message(self, message: Message) -> SendOp<Ready> {
        let SendOp { inner, .. } = self;
        SendOp {
            inner: SendOpStorage {
                handle: inner.handle,
                admission: inner.admission,
                kind: inner.kind,
                target: inner.target,
                parts: {
                    let mut parts = inner.parts;
                    parts.push(message);
                    parts
                },
                flags: inner.flags,
            },
            _state: PhantomData,
        }
    }
}

impl SendOp<Ready> {
    /// Adds another message part. The part is consumed on a successful submit
    /// (see [`SendOp`]).
    pub fn message(mut self, message: Message) -> Self {
        self.inner.parts.push(message);
        self
    }

    /// Sets the send flags applied at submit time.
    pub fn flags(self, flags: SendFlags) -> Self {
        let mut operation = self;
        operation.inner.flags = flags;
        operation
    }

    /// Submits the accumulated parts.
    pub fn submit(self) -> Result<bool, SubmitError> {
        crate::operations::submit_send(self.inner)
    }
}

impl RoutedSendOp<Empty> {
    /// Adds the first routed message part.
    pub fn message(self, message: Message) -> RoutedSendOp<Ready> {
        let RoutedSendOp { inner, .. } = self;
        RoutedSendOp {
            inner: RoutedSendOpStorage {
                admission: inner.admission,
                target: inner.target,
                parts: {
                    let mut parts = inner.parts;
                    parts.push(message);
                    parts
                },
            },
            _state: PhantomData,
        }
    }
}

impl RoutedSendOp<Ready> {
    /// Adds another routed message part.
    pub fn message(mut self, message: Message) -> Self {
        self.inner.parts.push(message);
        self
    }

    /// Starts the exact-target HWM-managed send.
    ///
    /// The returned Future never performs a blocking native submit. When Core
    /// reports backpressure it yields `Pending` until that exact physical
    /// target becomes writable, disconnects, closes, or reaches the socket's
    /// absolute send deadline.
    pub fn submit(self) -> impl std::future::Future<Output = Result<(), SubmitError>> + Send {
        crate::operations::submit_routed_send(self.inner)
    }
}

impl RequestOp<Empty> {
    /// Adds the first request part, transitioning the builder to the ready
    /// state. The part is consumed on a successful submit (see [`SendOp`]).
    pub fn message(self, message: Message) -> RequestOp<Ready> {
        let RequestOp { inner, .. } = self;
        RequestOp {
            inner: RequestOpStorage {
                admission: inner.admission,
                peer_rid: inner.peer_rid,
                parts: {
                    let mut parts = inner.parts;
                    parts.push(message);
                    parts
                },
                timeout: inner.timeout,
            },
            _state: PhantomData,
        }
    }
}

impl RequestOp<Ready> {
    /// Adds another request part. The part is consumed on a successful submit
    /// (see [`SendOp`]).
    pub fn message(mut self, message: Message) -> Self {
        self.inner.parts.push(message);
        self
    }

    /// Sets how long the request waits for a reply before timing out.
    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.inner.timeout = timeout;
        self
    }

    /// Starts the exact-target HWM-managed request.
    ///
    /// Admission wait and reply wait share the same absolute timeout. Submit
    /// failures are returned as [`ZlinkError::Submit`], while accepted-request
    /// completion failures are returned as [`ZlinkError::Request`].
    pub fn submit(
        self,
    ) -> impl std::future::Future<Output = Result<Vec<Message>, ZlinkError>> + Send {
        crate::operations::submit_routed_request(self.inner)
    }
}

impl ReplyOp<Empty> {
    /// Adds the first reply part, transitioning the builder to the ready state.
    /// The part is consumed on a successful submit (see [`SendOp`]).
    pub fn message(self, message: Message) -> ReplyOp<Ready> {
        let ReplyOp { inner, .. } = self;
        ReplyOp {
            inner: ReplyOpStorage {
                admission: inner.admission,
                kind: inner.kind,
                parts: {
                    let mut parts = inner.parts;
                    parts.push(message);
                    parts
                },
                flags: inner.flags,
            },
            _state: PhantomData,
        }
    }
}

impl ReplyOp<Ready> {
    /// Adds another reply part. The part is consumed on a successful submit
    /// (see [`SendOp`]).
    pub fn message(mut self, message: Message) -> Self {
        self.inner.parts.push(message);
        self
    }

    /// Sets the send flags applied at submit time.
    pub fn flags(mut self, flags: SendFlags) -> Self {
        self.inner.flags = flags;
        self
    }

    /// Submits the accumulated reply parts.
    pub fn submit(self) -> Result<(), SubmitError> {
        crate::operations::submit_reply(self.inner)
    }
}
