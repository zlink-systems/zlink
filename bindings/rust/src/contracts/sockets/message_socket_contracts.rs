// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, DealerSocketOptions,
    Received, RecvError, RecvFlags,
};
use crate::{Empty, RequestOp, RoutedSendOp, RoutingId, SendOp};

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
        let inner = crate::socket::pair_inner(self);
        crate::operations::socket_send_op(inner.handle, inner.send_completions.clone())
    }

    /// Receives a message into caller-provided `out` storage, reusable across
    /// calls.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        crate::socket::pair_inner(self).recv(out, flags)
    }

    /// Receives while retaining each physical part's Core HWM credit in
    /// `out`. Reusing, closing, consuming, or dropping `out` releases it.
    pub fn recv_retained(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        crate::socket::pair_inner(self).recv_retained(out, flags)
    }


    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(crate::socket::pair_inner(self))
    }

    /// Closes the socket and releases its native resources; further operations
    /// fail.
    pub fn close(&mut self) -> Result<(), crate::error::CloseError> {
        crate::socket::pair_inner_mut(self).close()
    }

    /// Binds the socket to a local transport address (for example
    /// `tcp://*:5555`, `ipc:///tmp/s`, or `inproc://name`) to accept
    /// connections there.
    pub fn bind(&self, addr: &str) -> Result<(), BindError> {
        crate::socket::pair_inner(self).bind(addr)
    }

    /// Stops accepting connections at `addr`, a previously bound address.
    pub fn unbind(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::pair_inner(self).unbind(addr)
    }

    /// Returns the concrete endpoint the socket last bound to, for example the
    /// resolved port after binding to a wildcard.
    pub fn last_endpoint(&self) -> Result<String, ConfigError> {
        crate::socket::pair_inner(self).last_endpoint()
    }

    /// Sets the server certificate (PEM) path for TLS. Apply before binding.
    pub fn set_tls_cert(&self, cert: &str) -> Result<(), ConfigError> {
        crate::socket::pair_inner(self).set_tls_cert(cert)
    }

    /// Sets the private key (PEM) path for the TLS server certificate. Apply
    /// before binding.
    pub fn set_tls_key(&self, key: &str) -> Result<(), ConfigError> {
        crate::socket::pair_inner(self).set_tls_key(key)
    }

    /// Sets the CA bundle (PEM) path used to verify the TLS peer. Apply before
    /// connecting.
    pub fn set_tls_ca(&self, ca_cert: &str) -> Result<(), ConfigError> {
        crate::socket::pair_inner(self).set_tls_ca(ca_cert)
    }

    /// Sets the expected server hostname to verify during the TLS handshake.
    pub fn set_tls_hostname(&self, hostname: &str) -> Result<(), ConfigError> {
        crate::socket::pair_inner(self).set_tls_hostname(hostname)
    }

    /// Sets whether to also trust the host system's CA store in addition to any
    /// configured CA bundle.
    pub fn set_tls_trust_system(&self, trust_system: bool) -> Result<(), ConfigError> {
        crate::socket::pair_inner(self).set_tls_trust_system(trust_system)
    }

    /// Configures this socket as a TLS server in one call. Apply before binding.
    ///
    /// `cert` is the server certificate (PEM) path, `key` its private key path,
    /// and `require_client_cert` requires and verifies a client certificate
    /// (mutual TLS).
    pub fn set_tls_server(
        &self,
        cert: &str,
        key: &str,
        require_client_cert: bool,
    ) -> Result<(), ConfigError> {
        crate::socket::pair_inner(self).set_tls_server(cert, key, require_client_cert)
    }

    /// Configures this socket as a TLS client in one call. Apply before
    /// connecting.
    ///
    /// `ca_cert` is the CA bundle (PEM) path used to verify the server,
    /// `hostname` the expected server hostname, and `trust_system` also trusts
    /// the host system's CA store.
    pub fn set_tls_client(
        &self,
        ca_cert: &str,
        hostname: &str,
        trust_system: bool,
    ) -> Result<(), ConfigError> {
        crate::socket::pair_inner(self).set_tls_client(ca_cert, hostname, trust_system)
    }

    /// Connects to a remote transport address (for example `tcp://host:5555`).
    /// Connection is asynchronous; this returns before the peer is reachable.
    pub fn connect(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::pair_inner(self).connect(addr)
    }

    /// Disconnects the connection previously established to `addr`.
    pub fn disconnect(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::pair_inner(self).disconnect(addr)
    }

    /// Disconnects the peer identified by `peer_rid` rather than by address.
    pub fn disconnect_rid(&self, peer_rid: &crate::message::RoutingId) -> Result<(), ConnectError> {
        crate::socket::pair_inner(self).disconnect_rid(peer_rid)
    }
}

impl DealerSocket {
    /// Begins a multipart send: add parts on the returned builder, then submit.
    /// A part is consumed on a successful submit (see [`SendOp`]).
    pub fn send(&self) -> RoutedSendOp<Empty> {
        let inner = crate::socket::dealer_inner(self);
        crate::operations::dealer_routed_send_op(
            inner.routed_handle.as_ref().expect("DEALER handle").clone(),
            inner
                .send_completions
                .as_ref()
                .expect("DEALER send completions")
                .clone(),
        )
    }

    /// Receives a message into caller-provided `out` storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn recv(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        crate::socket::dealer_inner(self).recv(out, flags)
    }

    /// Receives while retaining each physical part's Core HWM credit in
    /// `out`. Reusing, closing, consuming, or dropping `out` releases it.
    pub fn recv_retained(&self, out: &mut Received, flags: RecvFlags) -> Result<bool, RecvError> {
        crate::socket::dealer_inner(self).recv_dealer_retained(out, flags)
    }

    /// Begins a request: add parts on the returned builder, then submit and
    /// await a reply. Parts are consumed on a successful submit (see
    /// [`SendOp`]).
    pub fn request(&self) -> RequestOp<Empty> {
        crate::operations::dealer_request_op(
            crate::socket::dealer_inner(self)
                .routed_handle
                .as_ref()
                .expect("DEALER handle")
                .clone(),
        )
    }


    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(crate::socket::dealer_inner(self))
    }

    /// Returns the DEALER-specific typed options facade.
    pub fn dealer_options(&self) -> DealerSocketOptions<'_> {
        DealerSocketOptions::new(self)
    }

    /// Closes the socket and releases its native resources; further operations
    /// fail.
    pub fn close(&mut self) -> Result<(), crate::error::CloseError> {
        crate::socket::dealer_inner_mut(self).close()
    }

    /// Binds the socket to a local transport address (for example
    /// `tcp://*:5555`) to accept connections there.
    pub fn bind(&self, addr: &str) -> Result<(), BindError> {
        crate::socket::dealer_inner(self).bind(addr)
    }

    /// Stops accepting connections at `addr`, a previously bound address.
    pub fn unbind(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::dealer_inner(self).unbind(addr)
    }

    /// Returns the concrete endpoint the socket last bound to, for example the
    /// resolved port after binding to a wildcard.
    pub fn last_endpoint(&self) -> Result<String, ConfigError> {
        crate::socket::dealer_inner(self).last_endpoint()
    }

    /// Sets the server certificate (PEM) path for TLS. Apply before binding.
    pub fn set_tls_cert(&self, cert: &str) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self).set_tls_cert(cert)
    }

    /// Sets the private key (PEM) path for the TLS server certificate. Apply
    /// before binding.
    pub fn set_tls_key(&self, key: &str) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self).set_tls_key(key)
    }

    /// Sets the CA bundle (PEM) path used to verify the TLS peer. Apply before
    /// connecting.
    pub fn set_tls_ca(&self, ca_cert: &str) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self).set_tls_ca(ca_cert)
    }

    /// Sets the expected server hostname to verify during the TLS handshake.
    pub fn set_tls_hostname(&self, hostname: &str) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self).set_tls_hostname(hostname)
    }

    /// Sets whether to also trust the host system's CA store in addition to any
    /// configured CA bundle.
    pub fn set_tls_trust_system(&self, trust_system: bool) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self).set_tls_trust_system(trust_system)
    }

    /// Configures this socket as a TLS server in one call. Apply before binding.
    ///
    /// `cert` is the server certificate (PEM) path, `key` its private key path,
    /// and `require_client_cert` requires and verifies a client certificate
    /// (mutual TLS).
    pub fn set_tls_server(
        &self,
        cert: &str,
        key: &str,
        require_client_cert: bool,
    ) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self).set_tls_server(cert, key, require_client_cert)
    }

    /// Configures this socket as a TLS client in one call. Apply before
    /// connecting.
    ///
    /// `ca_cert` is the CA bundle (PEM) path used to verify the server,
    /// `hostname` the expected server hostname, and `trust_system` also trusts
    /// the host system's CA store.
    pub fn set_tls_client(
        &self,
        ca_cert: &str,
        hostname: &str,
        trust_system: bool,
    ) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self).set_tls_client(ca_cert, hostname, trust_system)
    }

    /// Connects to a remote transport address. Connection is asynchronous; this
    /// returns before the peer is reachable.
    pub fn connect(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::dealer_inner(self).connect(addr)
    }

    /// Disconnects the connection previously established to `addr`.
    pub fn disconnect(&self, addr: &str) -> Result<(), ConnectError> {
        crate::socket::dealer_inner(self).disconnect(addr)
    }

    /// Disconnects the peer identified by `peer_rid` rather than by address.
    pub fn disconnect_rid(&self, peer_rid: &RoutingId) -> Result<(), ConnectError> {
        crate::socket::dealer_inner(self).disconnect_rid(peer_rid)
    }

    /// Sets the routing id that identifies this DEALER to its peers. Apply
    /// before connecting so peers observe it from the first message.
    pub fn set_routing_id(&self, id: &RoutingId) -> Result<(), ConfigError> {
        crate::socket::dealer_inner(self).set_routing_id(id)
    }

    /// Returns the routing id that identifies this DEALER to its peers.
    pub fn routing_id(&self) -> Result<RoutingId, ConfigError> {
        crate::socket::dealer_inner(self).routing_id()
    }
}
