// SPDX-License-Identifier: MPL-2.0

use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, Empty, Received, RecvError,
    RecvFlags, ReplyOp, ReplyToken, RequestOp, RouterRoute, RouterSocket, RouterSocketOptions,
    RoutingId, SendOp,
};

impl RouterSocket {
    /// Begins a multipart send addressed to `target`: add parts on the returned
    /// builder, then submit. A part is consumed on a successful submit (see
    /// [`SendOp`]).
    pub fn send(&self, target: &RoutingId) -> SendOp<Empty> {
        let inner = &self.inner;
        crate::contracts::messaging::operations::SendOpStorage::routed_send_op(
            inner.routed_handle.as_ref().expect("ROUTER handle").clone(),
            inner
                .completion_owner
                .as_ref()
                .expect("ROUTER completion owner")
                .clone(),
            *target,
        )
    }

    /// Receives a routed message into caller-provided `out` storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        self.inner.recv_router_once(flags.bits(), out)
    }

    /// Returns the Core-selected route of every routing id as one atomic
    /// snapshot. A routing id without a row has no selected route. A
    /// successful snapshot clears [`crate::POLLROUTE`] readiness unless a
    /// later change raced with it. Call it only from the socket's single
    /// route observer.
    pub fn routes_snapshot(&self) -> Result<Vec<RouterRoute>, ConfigError> {
        self.inner.router_routes_snapshot()
    }

    /// Begins a request addressed to peer `peer_rid`: add parts, then submit and
    /// await a reply. Parts are consumed on a successful submit (see [`SendOp`]).
    pub fn request(&self, peer_rid: &RoutingId) -> RequestOp<Empty> {
        self.inner.router_request_op(*peer_rid)
    }

    /// Begins a reply to the request token from peer `rid`: add parts,
    /// then submit. Parts are consumed on a successful submit (see [`SendOp`]).
    pub fn reply(&self, rid: &RoutingId, reply_token: ReplyToken) -> ReplyOp<Empty> {
        let inner = &self.inner;
        crate::contracts::messaging::operations::ReplyOpStorage::router_reply_op(
            inner.routed_handle.as_ref().expect("ROUTER handle").clone(),
            inner
                .reply_owner
                .as_ref()
                .expect("ROUTER reply owner")
                .clone(),
            *rid,
            reply_token,
        )
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(&self.inner)
    }

    /// Returns the ROUTER-specific typed options facade.
    pub fn router_options(&self) -> RouterSocketOptions<'_> {
        RouterSocketOptions::new(&self.inner)
    }

    /// Sets the routing id that identifies this ROUTER to its peers. Apply
    /// before connecting so peers observe it from the first message.
    pub fn set_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError> {
        self.inner.set_routing_id(id)
    }

    /// Returns the routing id that identifies this ROUTER to its peers.
    pub fn routing_id(&self) -> Result<RoutingId, ConfigError> {
        self.inner.routing_id()
    }
}

impl_socket_common_base!(RouterSocket);

impl_socket_connections!(RouterSocket);
