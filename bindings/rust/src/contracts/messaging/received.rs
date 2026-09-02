use std::ffi::c_void;
use std::hash::{Hash, Hasher};
use std::sync::Arc;

use crate::error::{CloseError, RecvError, RecvResult};
use crate::message::{Message, RoutingId};
use crate::messaging_operations::{Empty, ReplyOp, SendOp};

/// Opaque capability for replying to one ROUTER request.
#[derive(Clone)]
pub struct ReplyToken {
    owner: Arc<crate::internal::RouterOwnerTag>,
    value: u64,
}

impl ReplyToken {
    pub(crate) fn from_native(owner: Arc<crate::internal::RouterOwnerTag>, value: u64) -> Self {
        Self { owner, value }
    }

    pub(crate) fn owner_matches(&self, owner: &Arc<crate::internal::RouterOwnerTag>) -> bool {
        Arc::ptr_eq(&self.owner, owner)
    }

    pub(crate) fn value(&self) -> u64 {
        self.value
    }
}

impl PartialEq for ReplyToken {
    fn eq(&self, other: &Self) -> bool {
        self.value == other.value && Arc::ptr_eq(&self.owner, &other.owner)
    }
}

impl Eq for ReplyToken {}

impl Hash for ReplyToken {
    fn hash<H: Hasher>(&self, state: &mut H) {
        (Arc::as_ptr(&self.owner) as usize).hash(state);
        self.value.hash(state);
    }
}

impl std::fmt::Debug for ReplyToken {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("ReplyToken")
    }
}

#[derive(Clone)]
pub(crate) struct ReceivedRoute {
    handle: *mut c_void,
    routed: Option<Arc<crate::internal::RoutedHandle>>,
    completion_owner: Arc<crate::internal::CompletionOwner>,
}

unsafe impl Send for ReceivedRoute {}

pub(crate) type ReceivedSendTarget = (
    *mut c_void,
    Option<Arc<crate::internal::RoutedHandle>>,
    Arc<crate::internal::CompletionOwner>,
    RoutingId,
);

/// A reusable received-message envelope.
pub struct Received {
    parts: Vec<Message>,
    routing_id: Option<RoutingId>,
    route: Option<ReceivedRoute>,
    reply_token: Option<ReplyToken>,
    receive_scratch: Vec<Message>,
}

impl Default for Received {
    fn default() -> Self {
        Self::empty()
    }
}

impl Received {
    pub fn empty() -> Self {
        Self {
            parts: Vec::new(),
            routing_id: None,
            route: None,
            reply_token: None,
            receive_scratch: Vec::new(),
        }
    }

    pub(crate) fn receive_scratch(&mut self) -> &mut Vec<Message> {
        &mut self.receive_scratch
    }

    pub(crate) fn replace_received_parts(&mut self, routing_id: Option<RoutingId>) {
        self.routing_id = routing_id;
        self.route = None;
        self.reply_token = None;
        std::mem::swap(&mut self.parts, &mut self.receive_scratch);
    }

    pub(crate) fn replace_router_parts(
        &mut self,
        handle: *mut c_void,
        routed: Arc<crate::internal::RoutedHandle>,
        completion_owner: Arc<crate::internal::CompletionOwner>,
        reply_owner: Arc<crate::internal::RouterOwnerTag>,
        routing_id: RoutingId,
        reply_token: u64,
    ) {
        self.route = Some(ReceivedRoute {
            handle,
            routed: Some(routed),
            completion_owner,
        });
        self.routing_id = Some(routing_id);
        self.reply_token =
            (reply_token != 0).then(|| ReplyToken::from_native(reply_owner, reply_token));
        std::mem::swap(&mut self.parts, &mut self.receive_scratch);
    }

    pub fn is_single_part(&self) -> bool {
        self.parts.len() == 1
    }
    pub fn routing_id(&self) -> Option<&RoutingId> {
        self.routing_id.as_ref()
    }
    pub fn reply_token(&self) -> Option<ReplyToken> {
        self.reply_token.clone()
    }
    pub fn parts(&self) -> &[Message] {
        &self.parts
    }
    pub fn first_part(&self) -> Result<&Message, RecvError> {
        self.parts.first().ok_or_else(recv_state_error)
    }
    pub fn single_part(self) -> Result<Message, RecvError> {
        self.single_part_or_error()
    }
    pub fn single_part_or_error(mut self) -> Result<Message, RecvError> {
        if self.parts.len() != 1 {
            return Err(recv_state_error());
        }
        Ok(std::mem::take(&mut self.parts)
            .into_iter()
            .next()
            .expect("single part"))
    }
    pub fn into_parts(mut self) -> Vec<Message> {
        std::mem::take(&mut self.parts)
    }
    pub fn close(mut self) -> Result<(), CloseError> {
        for part in &mut self.parts {
            part.close_now();
        }
        Ok(())
    }
    pub fn reply(&self) -> ReplyOp<Empty> {
        crate::received_operations::received_reply(self)
    }
    pub fn send(&self) -> SendOp<Empty> {
        crate::received_operations::received_send(self)
    }

    pub(crate) fn set_stream_send_context(
        &mut self,
        handle: *mut c_void,
        completion_owner: Arc<crate::internal::CompletionOwner>,
        routing_id: RoutingId,
    ) {
        self.route = Some(ReceivedRoute {
            handle,
            routed: None,
            completion_owner,
        });
        self.routing_id = Some(routing_id);
    }

    pub(crate) fn send_target(&self) -> Option<ReceivedSendTarget> {
        let route = self.route.as_ref()?;
        Some((
            route.handle,
            route.routed.clone(),
            Arc::clone(&route.completion_owner),
            *self.routing_id.as_ref()?,
        ))
    }

    pub(crate) fn reply_target(
        &self,
    ) -> Option<(
        Arc<crate::internal::RoutedHandle>,
        Arc<crate::internal::RouterOwnerTag>,
        RoutingId,
        ReplyToken,
    )> {
        let route = self.route.as_ref()?;
        let token = self.reply_token.clone()?;
        Some((
            route.routed.as_ref()?.clone(),
            token.owner.clone(),
            *self.routing_id.as_ref()?,
            token,
        ))
    }
}

pub(crate) fn recv_state_error() -> RecvError {
    RecvError::new(RecvResult::Busy, libc::EINVAL)
}
