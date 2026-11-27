// SPDX-License-Identifier: MPL-2.0

macro_rules! impl_socket_common_base {
    ($ty:ident) => {
        impl $ty {
            /// Closes the socket and releases its native resources; further
            /// operations fail.
            pub fn close(&mut self) -> Result<(), crate::contracts::errors::errors::CloseError> {
                self.inner.close()
            }

            /// Binds the socket to a local transport address (for example
            /// `tcp://*:5555`) to accept connections there.
            pub fn bind(&self, addr: &str) -> Result<(), BindError> {
                self.inner.bind(addr)
            }

            /// Stops accepting connections at `addr`, a previously bound address.
            pub fn unbind(&self, addr: &str) -> Result<(), ConnectError> {
                self.inner.unbind(addr)
            }

            /// Returns the concrete endpoint the socket last bound to, for
            /// example the resolved port after binding to a wildcard.
            pub fn last_endpoint(&self) -> Result<String, ConfigError> {
                self.inner.last_endpoint()
            }

            /// Sets the server certificate (PEM) path for TLS. Apply before
            /// binding.
            pub fn set_tls_cert(&self, cert: &str) -> Result<(), ConfigError> {
                self.inner.set_tls_cert(cert)
            }

            /// Sets the private key (PEM) path for the TLS server certificate.
            /// Apply before binding.
            pub fn set_tls_key(&self, key: &str) -> Result<(), ConfigError> {
                self.inner.set_tls_key(key)
            }

            /// Sets the CA bundle (PEM) path used to verify the TLS peer. Apply
            /// before connecting.
            pub fn set_tls_ca(&self, ca_cert: &str) -> Result<(), ConfigError> {
                self.inner.set_tls_ca(ca_cert)
            }

            /// Sets the expected server hostname to verify during the TLS
            /// handshake.
            pub fn set_tls_hostname(&self, hostname: &str) -> Result<(), ConfigError> {
                self.inner.set_tls_hostname(hostname)
            }

            /// Sets whether to also trust the host system's CA store in addition
            /// to any configured CA bundle.
            pub fn set_tls_trust_system(&self, trust_system: bool) -> Result<(), ConfigError> {
                self.inner.set_tls_trust_system(trust_system)
            }

            /// Configures this socket as a TLS server in one call. Apply before
            /// binding. `cert` is the certificate (PEM) path, `key` its private
            /// key path, and `require_client_cert` requires mutual TLS.
            pub fn set_tls_server(
                &self,
                cert: &str,
                key: &str,
                require_client_cert: bool,
            ) -> Result<(), ConfigError> {
                self.inner.set_tls_server(cert, key, require_client_cert)
            }

            /// Configures this socket as a TLS client in one call. Apply before
            /// connecting. `ca_cert` is the CA bundle (PEM) path, `hostname` the
            /// expected server hostname, and `trust_system` also trusts the
            /// system CA store.
            pub fn set_tls_client(
                &self,
                ca_cert: &str,
                hostname: &str,
                trust_system: bool,
            ) -> Result<(), ConfigError> {
                self.inner.set_tls_client(ca_cert, hostname, trust_system)
            }
        }
    };
}

macro_rules! impl_socket_connections {
    ($ty:ident) => {
        impl $ty {
            /// Connects to a remote transport address. Connection is
            /// asynchronous; this returns before the peer is reachable.
            pub fn connect(&self, addr: &str) -> Result<(), ConnectError> {
                self.inner.connect(addr)
            }

            /// Disconnects the connection previously established to `addr`.
            pub fn disconnect(&self, addr: &str) -> Result<(), ConnectError> {
                self.inner.disconnect(addr)
            }

            /// Disconnects the peer identified by `peer_rid` rather than by
            /// address.
            pub fn disconnect_rid(
                &self,
                peer_rid: &crate::contracts::messaging::message::RoutingId,
            ) -> Result<(), ConnectError> {
                self.inner.disconnect_rid(peer_rid)
            }
        }
    };
}
