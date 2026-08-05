// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, Empty, HandlerError, Message,
    Received, RecvError, RecvFlags, RoutingId, SendOp, StreamSocketOptions,
};

/// STREAM socket: exchanges framed packets with raw TCP peers addressed by
/// routing id.
pub struct StreamSocket {
    pub(crate) inner: Box<SocketStorage>,
}

impl std::panic::UnwindSafe for StreamSocket {}
impl std::panic::RefUnwindSafe for StreamSocket {}

impl StreamSocket {
    /// Begins a multipart send addressed to `target`: add parts on the returned
    /// builder, then submit. A part is consumed on a successful submit (see
    /// [`SendOp`]).
    pub fn send(&self, target: &RoutingId) -> SendOp<Empty> {
        crate::operations::socket_send_to_op(crate::socket::stream_inner(self).handle, *target)
    }

    /// Receives a message into caller-provided `out` storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        let received = crate::socket::stream_inner(self).recv(out, flags)?;
        if received {
            if let Some(routing_id) = out.routing_id().copied() {
                out.set_router_send_context(crate::socket::stream_inner(self).handle, routing_id);
            }
        }
        Ok(received)
    }

    /// Disconnects the peer identified by `peer_rid`.
    pub fn disconnect_rid(&self, peer_rid: &RoutingId) -> Result<(), ConnectError> {
        crate::socket::stream_inner(self).disconnect_rid(peer_rid)
    }

    /// Registers a handler invoked for each inbound framed packet with the
    /// sender routing id, header, and body. The handler takes ownership of both
    /// messages (dropped when it returns) and runs on a background dispatch
    /// thread.
    pub fn on_packet<F>(&mut self, handler: F) -> Result<(), HandlerError>
    where
        F: Fn(RoutingId, Message, Message) + Send + 'static,
    {
        crate::socket::stream_on_packet(self, handler)
    }

    /// Registers a callback invoked when the socket can accept more sends after
    /// back-pressure. The callback runs on a background dispatch thread.
    pub fn on_send_ready<F>(&mut self, handler: F) -> Result<(), HandlerError>
    where
        F: Fn() + Send + 'static,
    {
        crate::socket::stream_inner_mut(self).on_send_ready(handler)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(crate::socket::stream_inner(self))
    }

    /// Returns the STREAM-specific typed options facade.
    pub fn stream_options(&self) -> StreamSocketOptions<'_> {
        StreamSocketOptions::new(crate::socket::stream_inner(self))
    }

    /// Closes the socket and releases its native resources; further operations
    /// fail.
    pub fn close(&mut self) -> Result<(), crate::error::CloseError> {
        crate::socket::stream_inner_mut(self).close()
    }

    /// Binds the socket to a local transport address (for example
    /// `tcp://*:5555`) to accept connections there.
    pub fn bind(&self, addr: &str) -> Result<(), BindError> {
        crate::socket::stream_inner(self).bind(addr)
    }

    /// Stops accepting connections at `addr`, a previously bound address.
    pub fn unbind(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::stream_inner(self).unbind(addr)
    }

    /// Returns the concrete endpoint the socket last bound to, for example the
    /// resolved port after binding to a wildcard.
    pub fn last_endpoint(&self) -> Result<String, ConfigError> {
        crate::socket::stream_inner(self).last_endpoint()
    }

    /// Sets the server certificate (PEM) path for TLS. Apply before binding.
    pub fn set_tls_cert(&self, cert: &str) -> Result<(), ConfigError> {
        crate::socket::stream_inner(self).set_tls_cert(cert)
    }

    /// Sets the private key (PEM) path for the TLS server certificate. Apply
    /// before binding.
    pub fn set_tls_key(&self, key: &str) -> Result<(), ConfigError> {
        crate::socket::stream_inner(self).set_tls_key(key)
    }

    /// Sets the CA bundle (PEM) path used to verify the TLS peer. Apply before
    /// connecting.
    pub fn set_tls_ca(&self, ca_cert: &str) -> Result<(), ConfigError> {
        crate::socket::stream_inner(self).set_tls_ca(ca_cert)
    }

    /// Sets the expected server hostname to verify during the TLS handshake.
    pub fn set_tls_hostname(&self, hostname: &str) -> Result<(), ConfigError> {
        crate::socket::stream_inner(self).set_tls_hostname(hostname)
    }

    /// Sets whether to also trust the host system's CA store in addition to any
    /// configured CA bundle.
    pub fn set_tls_trust_system(&self, trust_system: bool) -> Result<(), ConfigError> {
        crate::socket::stream_inner(self).set_tls_trust_system(trust_system)
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
        crate::socket::stream_inner(self).set_tls_server(cert, key, require_client_cert)
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
        crate::socket::stream_inner(self).set_tls_client(ca_cert, hostname, trust_system)
    }

    /// Sets the routing id that identifies this socket to its peers. Apply
    /// before connecting so peers observe it from the first packet.
    pub fn set_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError> {
        crate::socket::stream_inner(self).set_routing_id(id)
    }

    /// Returns the routing id that identifies this socket to its peers.
    pub fn routing_id(&self) -> Result<RoutingId, ConfigError> {
        crate::socket::stream_inner(self).routing_id()
    }
}
