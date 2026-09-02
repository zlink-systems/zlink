// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CloseError, CommonSocketOptions, ConfigError, ConnectError, Empty, Message,
    Received, RecvError, RecvFlags, RoutingId, SendOp, StreamSocketOptions,
};

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
    /// builder, then submit. A part is consumed on a successful submit (see
    /// [`SendOp`]).
    pub fn send(&self, target: &RoutingId) -> SendOp<Empty> {
        let inner = crate::socket::stream_inner(self);
        crate::operations::stream_send_to_op(
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
        let received = crate::socket::stream_inner(self).recv(out, flags)?;
        if received {
            if let Some(routing_id) = out.routing_id().copied() {
                let inner = crate::socket::stream_inner(self);
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
        crate::socket::recv_stream_packet(
            crate::socket::stream_inner(self).handle,
            out,
            flags.bits(),
        )
    }

    /// Disconnects the peer identified by `peer_rid`.
    pub fn disconnect_rid(&self, peer_rid: &RoutingId) -> Result<(), ConnectError> {
        crate::socket::stream_inner(self).disconnect_rid(peer_rid)
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
