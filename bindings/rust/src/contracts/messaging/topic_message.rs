// SPDX-License-Identifier: MPL-2.0

use crate::RecvError;
use crate::domain::recv_state_error;
use crate::error::CloseError;
use crate::message::{Message, RoutingId};

/// A received publish: its topic, source routing id, and message parts.
///
/// Owns its parts until the envelope is dropped or [`close`](TopicMessage::close)d.
pub struct TopicMessage {
    /// The source routing id, when the receive path provides one.
    routing_id: Option<RoutingId>,
    /// The topic the message was published under.
    topic: smol_str::SmolStr,
    /// The message parts, owned by this envelope.
    parts: Vec<Message>,
    receive_scratch: Vec<Message>,
}

impl TopicMessage {
    /// Creates an empty reusable envelope; reuse it across `subscribe` calls.
    pub fn empty() -> Self {
        Self {
            routing_id: None,
            topic: smol_str::SmolStr::default(),
            parts: Vec::new(),
            receive_scratch: Vec::new(),
        }
    }

    pub(crate) fn receive_scratch(&mut self) -> &mut Vec<Message> {
        &mut self.receive_scratch
    }

    pub(crate) fn replace_received_parts(
        &mut self,
        routing_id: Option<RoutingId>,
        topic: smol_str::SmolStr,
    ) {
        self.routing_id = routing_id;
        self.topic = topic;
        std::mem::swap(&mut self.parts, &mut self.receive_scratch);
    }

    /// Returns `true` when this publish carries exactly one part.
    pub fn is_single_part(&self) -> bool {
        self.parts.len() == 1
    }

    /// Returns the topic this message was published under.
    pub fn topic(&self) -> &str {
        self.topic.as_str()
    }

    /// Returns the source routing id, when present.
    pub fn routing_id(&self) -> Option<&RoutingId> {
        self.routing_id.as_ref()
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
}
