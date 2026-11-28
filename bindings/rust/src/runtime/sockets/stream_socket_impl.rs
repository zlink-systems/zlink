// SPDX-License-Identifier: MPL-2.0

use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, Empty, Received, RecvError,
    RecvFlags, RoutingId, SendOp, StreamPacket, StreamSocket, StreamSocketOptions,
};

impl StreamSocket {
    /// Begins a multipart send addressed to `target`: add parts on the returned
    /// builder, then submit. A part is consumed on a successful submit (see
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
