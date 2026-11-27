// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{CloseError, Message, RoutingId};

/// Reusable storage for one framed STREAM packet.
pub struct StreamPacket {
    routing_id: Option<RoutingId>,
    header: Option<Message>,
    body: Option<Message>,
}

impl Default for StreamPacket {
    fn default() -> Self {
        Self::empty()
    }
}

impl StreamPacket {
    pub fn empty() -> Self {
        Self {
            routing_id: None,
            header: None,
            body: None,
        }
    }
    pub fn is_empty(&self) -> bool {
        self.routing_id.is_none() && self.header.is_none() && self.body.is_none()
    }
    pub fn routing_id(&self) -> Option<&RoutingId> {
        self.routing_id.as_ref()
    }
    pub fn header(&self) -> Option<&Message> {
        self.header.as_ref()
    }
    pub fn body(&self) -> Option<&Message> {
        self.body.as_ref()
    }
    pub fn close(self) -> Result<(), CloseError> {
        Ok(())
    }

    pub(crate) fn reset(&mut self) {
        self.routing_id = None;
        self.header = None;
        self.body = None;
    }
    pub(crate) fn replace(&mut self, routing_id: RoutingId, header: Message, body: Message) {
        self.routing_id = Some(routing_id);
        self.header = Some(header);
        self.body = Some(body);
    }
}

/// STREAM socket: exchanges framed packets with raw TCP peers addressed by
/// routing id.
pub struct StreamSocket {
    pub(crate) inner: Box<SocketStorage>,
}

impl std::panic::UnwindSafe for StreamSocket {}
impl std::panic::RefUnwindSafe for StreamSocket {}
