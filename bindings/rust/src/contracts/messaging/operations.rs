use std::ffi::c_void;
use std::future::Future;
use std::marker::PhantomData;
use std::pin::Pin;
use std::sync::Arc;
use std::time::Duration;

use crate::contracts::errors::errors::{SubmitError, SubmitResult, ZlinkError};
use crate::contracts::messaging::message::{Message, RoutingId};
use crate::contracts::sockets::socket_options::SendFlags;
use crate::internal::MessageParts;

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
