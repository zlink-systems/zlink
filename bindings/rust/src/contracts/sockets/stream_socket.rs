// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, Empty, Received, RecvError,
    RecvFlags, SendOp, StreamSocketOptions,
};
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

impl StreamSocket {
    /// Begins a multipart send addressed to `target`: add parts on the returned
    /// builder, then submit. A part moves into the builder and is consumed by submit whatever the result (see
    /// [`SendOp`]).
    pub fn send(&self, target: &RoutingId) -> SendOp<Empty> {
        let inner = &self.inner;
        crate::contracts::messaging::operations::SendOpStorage::stream_send_to_op(
            inner.handle,
            inner
                .completion_owner
                .as_ref()
                .expect("STREAM completion owner")
                .clone(),
            *target,
        )
    }

    /// Receives a message into caller-provided `out` storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        let received = self.inner.recv(out, flags)?;
        if received {
            if let Some(routing_id) = out.routing_id().copied() {
                let inner = &self.inner;
                out.set_stream_send_context(
                    inner.handle,
                    inner
                        .completion_owner
                        .as_ref()
                        .expect("STREAM completion owner")
                        .clone(),
                    routing_id,
                );
            }
        }
        Ok(received)
    }

    /// Receives one framed packet into reusable caller-owned storage.
    pub fn recv_packet(&self, out: &mut StreamPacket, flags: RecvFlags) -> Result<bool, RecvError> {
        self.inner.recv_stream_packet(out, flags.bits())
    }

    /// Disconnects the peer identified by `peer_rid`.
    pub fn disconnect_rid(&self, peer_rid: &RoutingId) -> Result<(), ConnectError> {
        self.inner.disconnect_rid(peer_rid)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(&self.inner)
    }

    /// Returns the STREAM-specific typed options facade.
    pub fn stream_options(&self) -> StreamSocketOptions<'_> {
        StreamSocketOptions::new(&self.inner)
    }

    /// Sets the routing id that identifies this socket to its peers. Apply
    /// before connecting so peers observe it from the first packet.
    pub fn set_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError> {
        self.inner.set_routing_id(id)
    }

    /// Returns the routing id that identifies this socket to its peers.
    pub fn routing_id(&self) -> Result<RoutingId, ConfigError> {
        self.inner.routing_id()
    }
}

impl_socket_common_base!(StreamSocket);
