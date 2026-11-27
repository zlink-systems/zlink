// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, DealerSocketOptions, Received,
    RecvError, RecvFlags,
};
use crate::{Empty, RequestOp, RoutingId, SendOp};

/// PAIR socket, a bidirectional one-to-one messaging socket.
pub struct PairSocket {
    pub(crate) inner: Box<SocketStorage>,
}

impl std::panic::UnwindSafe for PairSocket {}
impl std::panic::RefUnwindSafe for PairSocket {}

/// DEALER socket, the asynchronous request/reply client-side socket.
pub struct DealerSocket {
    pub(crate) inner: Box<SocketStorage>,
}

impl std::panic::UnwindSafe for DealerSocket {}
impl std::panic::RefUnwindSafe for DealerSocket {}

impl PairSocket {
    /// Begins a multipart send: add parts on the returned builder, then submit.
    /// A part is consumed on a successful submit (see [`SendOp`]).
    pub fn send(&self) -> SendOp<Empty> {
        self.inner.socket_send_op()
    }

    /// Receives a message into caller-provided `out` storage, reusable across
    /// calls.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        self.inner.recv(out, flags)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(&self.inner)
    }
}

impl DealerSocket {
    /// Begins a multipart send: add parts on the returned builder, then submit.
    /// A part is consumed on a successful submit (see [`SendOp`]).
    pub fn send(&self) -> SendOp<Empty> {
        self.inner.dealer_send_op()
    }

    /// Receives a message into caller-provided `out` storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        self.inner.recv_dealer(out, flags)
    }

    /// Begins a request: add parts on the returned builder, then submit and
    /// await a reply. Parts are consumed on a successful submit (see
    /// [`SendOp`]).
    pub fn request(&self) -> RequestOp<Empty> {
        self.inner.dealer_request_op()
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(&self.inner)
    }

    /// Returns the DEALER-specific typed options facade.
    pub fn dealer_options(&self) -> DealerSocketOptions<'_> {
        DealerSocketOptions::new(self)
    }

    /// Sets the routing id that identifies this DEALER to its peers. Apply
    /// before connecting so peers observe it from the first message.
    pub fn set_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError> {
        self.inner.set_routing_id(id)
    }

    /// Returns the routing id that identifies this DEALER to its peers.
    pub fn routing_id(&self) -> Result<RoutingId, ConfigError> {
        self.inner.routing_id()
    }
}

impl_socket_common_base!(PairSocket);
impl_socket_connections!(PairSocket);

impl_socket_common_base!(DealerSocket);
impl_socket_connections!(DealerSocket);
