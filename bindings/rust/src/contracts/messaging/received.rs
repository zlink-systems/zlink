use std::ffi::c_void;

use crate::error::{CloseError, RecvError, RecvResult};
use crate::message::{Message, RoutingId};
use crate::messaging_operations::{Empty, ReplyOp, SendOp};

/// Route metadata and the native handle that owns the route.
///
/// Keeping these values together prevents the public routing id and the
/// private send/reply target from diverging.
#[derive(Clone, Copy)]
pub(crate) struct ReceivedRoute {
    handle: Option<*mut c_void>,
    routing_id: RoutingId,
    request_seq: Option<u64>,
}

unsafe impl Send for ReceivedRoute {}

/// A received message envelope: its routing metadata and message parts.
///
/// Owns its parts until the envelope is dropped or
/// [`close`](Received::close)d. Reuse one instance across `recv` calls to
/// avoid a per-receive allocation.
pub struct Received {
    /// The message parts, owned by this envelope.
    parts: Vec<Message>,
    route: Option<ReceivedRoute>,
    receive_scratch: Vec<Message>,
}

impl Default for Received {
    fn default() -> Self {
        Self::empty()
    }
}

impl Received {
    /// Create an empty `Received` for caller-provided storage.
    ///
    /// Hand the same instance to socket `recv` across calls to avoid the
    /// per-recv allocation; the binding overwrites the internal state on
    /// each successful receive.
    pub fn empty() -> Self {
        Self {
            parts: Vec::new(),
            route: None,
            receive_scratch: Vec::new(),
        }
    }

    pub(crate) fn receive_scratch(&mut self) -> &mut Vec<Message> {
        &mut self.receive_scratch
    }

    pub(crate) fn replace_received_parts(&mut self, routing_id: Option<RoutingId>) {
        self.route = routing_id.map(|routing_id| ReceivedRoute {
            handle: None,
            routing_id,
            request_seq: None,
        });
        std::mem::swap(&mut self.parts, &mut self.receive_scratch);
    }

    pub(crate) fn replace_router_parts(
        &mut self,
        handle: *mut c_void,
        routing_id: RoutingId,
        request_seq: u64,
    ) {
        self.route = Some(ReceivedRoute {
            handle: Some(handle),
            routing_id,
            request_seq: (request_seq != 0).then_some(request_seq),
        });
        std::mem::swap(&mut self.parts, &mut self.receive_scratch);
    }

    /// Returns `true` when the envelope carries exactly one part.
    pub fn is_single_part(&self) -> bool {
        self.parts.len() == 1
    }

    /// Returns the source routing id, when present.
    pub fn routing_id(&self) -> Option<&RoutingId> {
        self.route.as_ref().map(|route| &route.routing_id)
    }

    /// Returns the request sequence, present when this envelope can be replied
    /// to.
    pub fn request_seq(&self) -> Option<u64> {
        self.route.and_then(|route| route.request_seq)
    }

    /// Returns the message parts, owned by this envelope.
    pub fn parts(&self) -> &[Message] {
        &self.parts
    }

    /// Returns the first part without transferring ownership; errors when the
    /// envelope has no parts.
    pub fn first_part(&self) -> Result<&Message, RecvError> {
        self.parts.first().ok_or_else(recv_state_error)
    }

    /// Consumes the envelope and returns its only part, transferring ownership;
    /// errors unless it holds exactly one part.
    pub fn single_part(self) -> Result<Message, RecvError> {
        self.single_part_or_error()
    }

    /// Consumes the envelope and returns its only part, transferring ownership;
    /// errors unless it holds exactly one part.
    pub fn single_part_or_error(self) -> Result<Message, RecvError> {
        if self.parts.len() != 1 {
            return Err(recv_state_error());
        }
        Ok(self.parts.into_iter().next().expect("single part"))
    }

    /// Consumes the envelope and returns ownership of all its parts.
    pub fn into_parts(self) -> Vec<Message> {
        self.parts
    }

    /// Closes every part, releasing their payloads.
    pub fn close(mut self) -> Result<(), CloseError> {
        for part in &mut self.parts {
            part.close_now();
        }
        Ok(())
    }

    /// Begins a reply to this request: add parts on the returned builder, then
    /// submit. Parts are consumed on a successful submit (see [`SendOp`]). Only
    /// valid for replyable envelopes (those with a request sequence).
    pub fn reply(&self) -> ReplyOp<Empty> {
        crate::received_operations::received_reply(self)
    }

    /// Begins a send addressed to this envelope's source route: add parts, then
    /// submit. Parts are consumed on a successful submit (see [`SendOp`]).
    pub fn send(&self) -> SendOp<Empty> {
        crate::received_operations::received_send(self)
    }

    pub(crate) fn set_router_send_context(&mut self, handle: *mut c_void, routing_id: RoutingId) {
        self.route = Some(ReceivedRoute {
            handle: Some(handle),
            routing_id,
            request_seq: self.route.and_then(|route| route.request_seq),
        });
    }

    pub(crate) fn send_target(&self) -> Option<(*mut c_void, RoutingId)> {
        self.route
            .and_then(|route| route.handle.map(|handle| (handle, route.routing_id)))
    }

    pub(crate) fn reply_target(&self) -> Option<(*mut c_void, RoutingId, u64)> {
        self.route
            .and_then(|route| Some((route.handle?, route.routing_id, route.request_seq?)))
    }
}

pub(crate) fn recv_state_error() -> RecvError {
    RecvError::new(RecvResult::Busy, libc::EINVAL)
}
