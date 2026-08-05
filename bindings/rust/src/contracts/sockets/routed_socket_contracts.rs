// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, HandlerError, Received, RecvError,
    RecvFlags, RouterSocketOptions,
};
use crate::{Empty, ReplyOp, RequestOp, RoutingId, SendOp};

/// ROUTER socket: routes messages to peers addressed by routing id, the
/// server side of asynchronous request/reply.
pub struct RouterSocket {
    pub(crate) inner: Box<SocketStorage>,
}

impl std::panic::UnwindSafe for RouterSocket {}
impl std::panic::RefUnwindSafe for RouterSocket {}

impl RouterSocket {
    /// Begins a multipart send addressed to `target`: add parts on the returned
    /// builder, then submit. A part is consumed on a successful submit (see
    /// [`SendOp`]).
    pub fn send(&self, target: &RoutingId) -> SendOp<Empty> {
        crate::operations::socket_send_to_op(crate::socket::router_inner(self).handle, *target)
    }

    /// Receives a routed message into caller-provided `out` storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        crate::socket::recv_router_once(crate::socket::router_inner(self).handle, flags.bits(), out)
    }

    /// Begins a request addressed to peer `peer_rid`: add parts, then submit and
    /// await a reply. Parts are consumed on a successful submit (see [`SendOp`]).
    pub fn request(&self, peer_rid: &RoutingId) -> RequestOp<Empty> {
        crate::operations::router_request_op(crate::socket::router_inner(self).handle, *peer_rid)
    }

    /// Begins a reply to the request `request_seq` from peer `rid`: add parts,
    /// then submit. Parts are consumed on a successful submit (see [`SendOp`]).
    pub fn reply(&self, rid: &RoutingId, request_seq: u64) -> ReplyOp<Empty> {
        crate::operations::router_reply_op(
            crate::socket::router_inner(self).handle,
            *rid,
            request_seq,
        )
    }

    /// Registers a callback invoked when the socket can accept more sends after
    /// back-pressure. The callback runs on a background dispatch thread.
    pub fn on_send_ready<F>(&mut self, handler: F) -> Result<(), HandlerError>
    where
        F: Fn() + Send + 'static,
    {
        crate::socket::router_inner_mut(self).on_send_ready(handler)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(crate::socket::router_inner(self))
    }

    /// Returns the ROUTER-specific typed options facade.
    pub fn router_options(&self) -> RouterSocketOptions<'_> {
        RouterSocketOptions::new(crate::socket::router_inner(self))
    }

    /// Closes the socket and releases its native resources; further operations
    /// fail.
    pub fn close(&mut self) -> Result<(), crate::error::CloseError> {
        crate::socket::router_inner_mut(self).close()
    }

    /// Binds the socket to a local transport address (for example
    /// `tcp://*:5555`) to accept connections there.
    pub fn bind(&self, addr: &str) -> Result<(), BindError> {
        crate::socket::router_inner(self).bind(addr)
    }

    /// Stops accepting connections at `addr`, a previously bound address.
    pub fn unbind(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::router_inner(self).unbind(addr)
    }

    /// Returns the concrete endpoint the socket last bound to, for example the
    /// resolved port after binding to a wildcard.
    pub fn last_endpoint(&self) -> Result<String, ConfigError> {
        crate::socket::router_inner(self).last_endpoint()
    }

    /// Sets the server certificate (PEM) path for TLS. Apply before binding.
    pub fn set_tls_cert(&self, cert: &str) -> Result<(), ConfigError> {
        crate::socket::router_inner(self).set_tls_cert(cert)
    }

    /// Sets the private key (PEM) path for the TLS server certificate. Apply
    /// before binding.
    pub fn set_tls_key(&self, key: &str) -> Result<(), ConfigError> {
        crate::socket::router_inner(self).set_tls_key(key)
    }

    /// Sets the CA bundle (PEM) path used to verify the TLS peer. Apply before
    /// connecting.
    pub fn set_tls_ca(&self, ca_cert: &str) -> Result<(), ConfigError> {
        crate::socket::router_inner(self).set_tls_ca(ca_cert)
    }

    /// Sets the expected server hostname to verify during the TLS handshake.
    pub fn set_tls_hostname(&self, hostname: &str) -> Result<(), ConfigError> {
        crate::socket::router_inner(self).set_tls_hostname(hostname)
    }

    /// Sets whether to also trust the host system's CA store in addition to any
    /// configured CA bundle.
    pub fn set_tls_trust_system(&self, trust_system: bool) -> Result<(), ConfigError> {
        crate::socket::router_inner(self).set_tls_trust_system(trust_system)
    }

    /// Configures this socket as a TLS server in one call. Apply before binding.
    /// `cert` is the certificate (PEM) path, `key` its private key path, and
    /// `require_client_cert` requires mutual TLS.
    pub fn set_tls_server(
        &self,
        cert: &str,
        key: &str,
        require_client_cert: bool,
    ) -> Result<(), ConfigError> {
        crate::socket::router_inner(self).set_tls_server(cert, key, require_client_cert)
    }

    /// Configures this socket as a TLS client in one call. Apply before
    /// connecting. `ca_cert` is the CA bundle (PEM) path, `hostname` the
    /// expected server hostname, and `trust_system` also trusts the system CA
    /// store.
    pub fn set_tls_client(
        &self,
        ca_cert: &str,
        hostname: &str,
        trust_system: bool,
    ) -> Result<(), ConfigError> {
        crate::socket::router_inner(self).set_tls_client(ca_cert, hostname, trust_system)
    }

    /// Connects to a remote transport address. Connection is asynchronous; this
    /// returns before the peer is reachable.
    pub fn connect(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::router_inner(self).connect(addr)
    }

    /// Disconnects the connection previously established to `addr`.
    pub fn disconnect(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::router_inner(self).disconnect(addr)
    }

    /// Disconnects the peer identified by `peer_rid` rather than by address.
    pub fn disconnect_rid(&self, peer_rid: &RoutingId) -> Result<(), ConnectError> {
        crate::socket::router_inner(self).disconnect_rid(peer_rid)
    }

    /// Sets the routing id that identifies this ROUTER to its peers. Apply
    /// before connecting so peers observe it from the first message.
    pub fn set_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError> {
        crate::socket::router_inner(self).set_routing_id(id)
    }

    /// Returns the routing id that identifies this ROUTER to its peers.
    pub fn routing_id(&self) -> Result<RoutingId, ConfigError> {
        crate::socket::router_inner(self).routing_id()
    }
}
