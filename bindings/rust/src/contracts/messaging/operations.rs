use std::ffi::c_void;
use std::marker::PhantomData;
use std::time::Duration;

use crate::error::{RequestError, SubmitError};
use crate::flags::SendFlags;
use crate::message::{Message, RoutingId};

/// Typestate marker: no message has been set yet.
pub struct Empty;

/// Typestate marker: at least one message part has been set.
pub struct Ready;

/// Typestate marker: flags have been set, only callback submit available.
pub struct CallbackReady;

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
    pub(crate) kind: SendOpKind,
    pub(crate) target: Option<RoutingId>,
    pub(crate) parts: MessageParts,
    pub(crate) flags: SendFlags,
}

pub(crate) enum SendOpKind {
    Plain,
    Published { topic: smol_str::SmolStr },
    Routed,
}

unsafe impl Send for SendOpStorage {}

/// A request builder: add parts, then submit and await a reply. Parts are
/// consumed on a successful submit (see [`SendOp`]).
pub struct RequestOp<State> {
    pub(crate) inner: RequestOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct RequestOpStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) kind: RequestOpKind,
    pub(crate) peer_rid: Option<RoutingId>,
    pub(crate) parts: MessageParts,
    pub(crate) flags: Option<SendFlags>,
    pub(crate) timeout: Duration,
}

pub(crate) enum RequestOpKind {
    DealerRequest,
    RouterRequest,
}

unsafe impl Send for RequestOpStorage {}

/// A reply builder: add parts, then submit. Parts are consumed on a successful
/// submit (see [`SendOp`]).
pub struct ReplyOp<State> {
    pub(crate) inner: ReplyOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct ReplyOpStorage {
    pub(crate) handle: *mut c_void,
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

impl RequestOp<Empty> {
    /// Adds the first request part, transitioning the builder to the ready
    /// state. The part is consumed on a successful submit (see [`SendOp`]).
    pub fn message(self, message: Message) -> RequestOp<Ready> {
        let RequestOp { inner, .. } = self;
        RequestOp {
            inner: RequestOpStorage {
                handle: inner.handle,
                kind: inner.kind,
                peer_rid: inner.peer_rid,
                parts: {
                    let mut parts = inner.parts;
                    parts.push(message);
                    parts
                },
                flags: inner.flags,
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

    /// Sets the send flags applied at submit time.
    pub fn flags(self, flags: SendFlags) -> RequestOp<CallbackReady> {
        let mut operation = self;
        operation.inner.flags = Some(flags);
        RequestOp {
            inner: operation.inner,
            _state: PhantomData,
        }
    }

    /// Submits the request; the reply (or error) is delivered later to
    /// `callback`, which owns the reply parts.
    pub fn submit<F>(self, callback: F) -> Result<(), SubmitError>
    where
        F: FnOnce(Result<Vec<Message>, RequestError>) + Send + 'static,
    {
        crate::operations::submit_request(self.inner, SendFlags::NONE, callback)
    }
}

impl RequestOp<CallbackReady> {
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

    /// Sets the send flags applied at submit time.
    pub fn flags(mut self, flags: SendFlags) -> Self {
        self.inner.flags = Some(flags);
        self
    }

    /// Submits the request; the reply (or error) is delivered later to
    /// `callback`, which owns the reply parts.
    pub fn submit<F>(self, callback: F) -> Result<(), SubmitError>
    where
        F: FnOnce(Result<Vec<Message>, RequestError>) + Send + 'static,
    {
        let flags = self.inner.flags.unwrap_or(SendFlags::NONE);
        crate::operations::submit_request(self.inner, flags, callback)
    }
}

impl ReplyOp<Empty> {
    /// Adds the first reply part, transitioning the builder to the ready state.
    /// The part is consumed on a successful submit (see [`SendOp`]).
    pub fn message(self, message: Message) -> ReplyOp<Ready> {
        let ReplyOp { inner, .. } = self;
        ReplyOp {
            inner: ReplyOpStorage {
                handle: inner.handle,
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
