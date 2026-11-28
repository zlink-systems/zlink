// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, DealerSocketOptions, Empty,
    Received, RecvError, RecvFlags, RequestOp, RoutingId, SendOp,
};

/// DEALER socket, the asynchronous request/reply client-side socket.
pub struct DealerSocket {
    pub(crate) inner: Box<SocketStorage>,
}

impl std::panic::UnwindSafe for DealerSocket {}
impl std::panic::RefUnwindSafe for DealerSocket {}

impl DealerSocket {
    /// Begins a multipart send: add parts on the returned builder, then submit.
    /// A part moves into the builder and is consumed by submit whatever the result (see [`SendOp`]).
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
    /// await a reply. Parts move into the builder and are consumed by submit whatever the result (see
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

impl_socket_common_base!(DealerSocket);
impl_socket_connections!(DealerSocket);
