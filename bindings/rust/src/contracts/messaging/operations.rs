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

    /// Flattens the parts into one contiguous vector for the single-call
    /// `zlink_send_async` record submit.
    pub(crate) fn into_vec(self) -> Vec<Message> {
        let mut out = Vec::with_capacity(usize::from(self.first.is_some()) + self.rest.len());
        out.extend(self.first);
        out.extend(self.rest);
        out
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
/// HWM-managed send has two terminals. [`submit`](SendOp::submit) returns a
/// runtime-independent [`Future`](std::future::Future) whose completion is
/// driven by the Core send-completion callback. [`submit_blocking`](SendOp::submit_blocking)
/// completes synchronously and accepts [`SendFlags`]: [`SendFlags::NONE`]
/// blocks for admission and [`SendFlags::DONT_WAIT`] reports backpressure
/// immediately.
///
/// The `State` type parameter is a typestate ([`Empty`] or [`Ready`]) that
/// statically tracks whether at least one part has been added.
pub struct SendOp<State> {
    pub(crate) inner: SendOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct SendOpStorage {
    pub(crate) handle: *mut c_void,
    pub(crate) routed: Option<Arc<crate::internal::RoutedHandle>>,
    pub(crate) completions: Option<Arc<crate::internal::SendCompletions>>,
    pub(crate) kind: SendOpKind,
    pub(crate) target: Option<RoutingId>,
    pub(crate) parts: MessageParts,
    pub(crate) timeout: Duration,
}

pub(crate) enum SendOpKind {
    /// PAIR send. Core picks the single pipe.
    Plain,
    /// STREAM send addressed at one exact raw peer.
    StreamRouted,
    /// DEALER/ROUTER routed send addressed at one exact peer.
    Routed,
}

unsafe impl Send for SendOpStorage {}

/// A PUB/XPUB publish builder. Publish is lossy and never waits on a HWM, so
/// its terminal is synchronous. With `ZLINK_PUB_OPT_NODROP` a full subscriber surfaces an
/// immediate `Backpressured` error instead.
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

/// An HWM-managed routed send builder. Its [`submit`](RoutedSendOp::submit)
/// terminal returns a runtime-independent [`Future`](std::future::Future).
pub struct RoutedSendOp<State> {
    pub(crate) inner: RoutedSendOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct RoutedSendOpStorage {
    pub(crate) routed: Arc<crate::internal::RoutedHandle>,
    pub(crate) completions: Arc<crate::internal::SendCompletions>,
    pub(crate) target: Option<RoutingId>,
    pub(crate) parts: MessageParts,
    pub(crate) timeout: Duration,
}

/// A request builder: add parts, then submit and await a reply. Parts are
/// consumed on a successful submit (see [`SendOp`]).
pub struct RequestOp<State> {
    pub(crate) inner: RequestOpStorage,
    pub(crate) _state: PhantomData<State>,
}

pub(crate) struct RequestOpStorage {
    pub(crate) routed: Arc<crate::internal::RoutedHandle>,
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
    pub(crate) routed: Arc<crate::internal::RoutedHandle>,
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
        let SendOp { mut inner, .. } = self;
        inner.parts.push(message);
        SendOp {
            inner,
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

    /// Sets the per-operation deadline handed to Core. Zero (the default)
    /// falls back to the socket's `SNDTIMEO`.
    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.inner.timeout = timeout;
        self
    }

    /// Starts the HWM-managed send.
    ///
    /// The returned Future is runtime-independent and is completed by the Core
    /// send-completion callback. When Core admits the record immediately the
    /// first poll returns `Ready`. Dropping the Future before completion
    /// requests `zlink_send_async_cancel`; the operation still completes
    /// exactly once inside Core.
    pub fn submit(self) -> impl std::future::Future<Output = Result<(), SubmitError>> + Send {
        crate::operations::submit_send(self.inner)
    }

    /// Submits the HWM-managed send synchronously with the supplied flags.
    ///
    /// [`SendFlags::NONE`] blocks until Core admits the record (subject to the
    /// socket's `SNDTIMEO`). [`SendFlags::DONT_WAIT`] returns an immediate
    /// [`SubmitError`] when the outbound HWM is full.
    pub fn submit_blocking(self, flags: SendFlags) -> Result<(), SubmitError> {
        crate::operations::submit_send_blocking(self.inner, flags)
    }
}

impl PublishOp<Empty> {
    /// Adds the first message part, transitioning the builder to the ready
    /// state.
    pub fn message(self, message: Message) -> PublishOp<Ready> {
        let PublishOp { mut inner, .. } = self;
        inner.parts.push(message);
        PublishOp {
            inner,
            _state: PhantomData,
        }
    }
}

impl PublishOp<Ready> {
    /// Adds another message part.
    pub fn message(mut self, message: Message) -> Self {
        self.inner.parts.push(message);
        self
    }

    /// Sets the send flags applied at submit time.
    pub fn flags(mut self, flags: SendFlags) -> Self {
        self.inner.flags = flags;
        self
    }

    /// Publishes the accumulated parts and returns synchronously.
    ///
    /// Default PUB semantics are lossy: a subscriber at its HWM loses its copy
    /// and the publisher proceeds. With `ZLINK_PUB_OPT_NODROP` a full
    /// subscriber makes this call fail with `SubmitResult::Backpressured`, and
    /// the retry policy belongs to the application.
    pub fn submit(self) -> Result<(), SubmitError> {
        crate::operations::submit_publish(self.inner)
    }
}

impl RoutedSendOp<Empty> {
    /// Adds the first routed message part.
    pub fn message(self, message: Message) -> RoutedSendOp<Ready> {
        let RoutedSendOp { mut inner, .. } = self;
        inner.parts.push(message);
        RoutedSendOp {
            inner,
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

    /// Sets the per-operation deadline handed to Core. Zero (the default)
    /// falls back to the socket's `SNDTIMEO`.
    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.inner.timeout = timeout;
        self
    }

    /// Starts the HWM-managed routed send.
    ///
    /// The returned Future is runtime-independent and is completed by the Core
    /// send-completion callback: Core owns the HWM wait, the deadline and the
    /// retry. Dropping the Future before completion requests
    /// `zlink_send_async_cancel`.
    pub fn submit(self) -> impl std::future::Future<Output = Result<(), SubmitError>> + Send {
        crate::operations::submit_routed_send(self.inner)
    }

    /// Submits the HWM-managed routed send synchronously with the supplied
    /// flags.
    ///
    /// [`SendFlags::NONE`] blocks until Core admits the record (subject to the
    /// socket's `SNDTIMEO`). [`SendFlags::DONT_WAIT`] returns an immediate
    /// [`SubmitError`] when the outbound HWM is full.
    pub fn submit_blocking(self, flags: SendFlags) -> Result<(), SubmitError> {
        crate::operations::submit_routed_send_blocking(self.inner, flags)
    }
}

impl RequestOp<Empty> {
    /// Adds the first request part, transitioning the builder to the ready
    /// state. The part is consumed on a successful submit (see [`SendOp`]).
    pub fn message(self, message: Message) -> RequestOp<Ready> {
        let RequestOp { mut inner, .. } = self;
        inner.parts.push(message);
        RequestOp {
            inner,
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
        let ReplyOp { mut inner, .. } = self;
        inner.parts.push(message);
        ReplyOp {
            inner,
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
