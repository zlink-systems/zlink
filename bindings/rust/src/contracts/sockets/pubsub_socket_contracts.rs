// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, HandlerError, PubSocketOptions,
    RecvError, RecvFlags, SubSocketOptions, SubscriptionEvent, TopicMessage,
};
use crate::{Empty, SendOp};

macro_rules! define_pubsub_socket {
    ($name:ident, $doc:literal) => {
        #[doc = $doc]
        pub struct $name {
            pub(crate) inner: Box<SocketStorage>,
        }

        impl std::panic::UnwindSafe for $name {}
        impl std::panic::RefUnwindSafe for $name {}
    };
}

define_pubsub_socket!(
    PubSocket,
    "PUB socket: topic-filtered publish that drops messages with no matching subscriber."
);
define_pubsub_socket!(
    SubSocket,
    "SUB socket: a receive-only subscriber filtered by its subscriptions."
);
define_pubsub_socket!(
    XPubSocket,
    "XPUB socket: like PUB, but also surfaces subscriber subscription and unsubscription events."
);
define_pubsub_socket!(
    XSubSocket,
    "XSUB socket: a subscriber whose subscriptions are carried as messages."
);

macro_rules! impl_pubsub_common {
    ($ty:ident, $inner:path, $inner_mut:path) => {
        impl $ty {
            /// Closes the socket and releases its native resources; further
            /// operations fail.
            pub fn close(&mut self) -> Result<(), crate::error::CloseError> {
                $inner_mut(self).close()
            }

            /// Binds the socket to a local transport address (for example
            /// `tcp://*:5555`) to accept connections there.
            pub fn bind(&self, addr: &str) -> Result<(), BindError> {
                $inner(self).bind(addr)
            }

            /// Stops accepting connections at `addr`, a previously bound address.
            pub fn unbind(&self, addr: &str) -> Result<(), ConnectError> {
                $inner(self).unbind(addr)
            }

            /// Returns the concrete endpoint the socket last bound to, for
            /// example the resolved port after binding to a wildcard.
            pub fn last_endpoint(&self) -> Result<String, ConfigError> {
                $inner(self).last_endpoint()
            }

            /// Sets the server certificate (PEM) path for TLS. Apply before
            /// binding.
            pub fn set_tls_cert(&self, cert: &str) -> Result<(), ConfigError> {
                $inner(self).set_tls_cert(cert)
            }

            /// Sets the private key (PEM) path for the TLS server certificate.
            /// Apply before binding.
            pub fn set_tls_key(&self, key: &str) -> Result<(), ConfigError> {
                $inner(self).set_tls_key(key)
            }

            /// Sets the CA bundle (PEM) path used to verify the TLS peer. Apply
            /// before connecting.
            pub fn set_tls_ca(&self, ca_cert: &str) -> Result<(), ConfigError> {
                $inner(self).set_tls_ca(ca_cert)
            }

            /// Sets the expected server hostname to verify during the TLS
            /// handshake.
            pub fn set_tls_hostname(&self, hostname: &str) -> Result<(), ConfigError> {
                $inner(self).set_tls_hostname(hostname)
            }

            /// Sets whether to also trust the host system's CA store in addition
            /// to any configured CA bundle.
            pub fn set_tls_trust_system(&self, trust_system: bool) -> Result<(), ConfigError> {
                $inner(self).set_tls_trust_system(trust_system)
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
                $inner(self).set_tls_server(cert, key, require_client_cert)
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
                $inner(self).set_tls_client(ca_cert, hostname, trust_system)
            }

            /// Connects to a remote transport address. Connection is
            /// asynchronous; this returns before the peer is reachable.
            pub fn connect(&self, addr: &str) -> Result<(), ConnectError> {
                $inner(self).connect(addr)
            }

            /// Disconnects the connection previously established to `addr`.
            pub fn disconnect(&self, addr: &str) -> Result<(), ConnectError> {
                $inner(self).disconnect(addr)
            }

            /// Disconnects the peer identified by `peer_rid` rather than by
            /// address.
            pub fn disconnect_rid(
                &self,
                peer_rid: &crate::message::RoutingId,
            ) -> Result<(), ConnectError> {
                $inner(self).disconnect_rid(peer_rid)
            }
        }
    };
}

impl PubSocket {
    /// Begins publishing under `topic`: add parts on the returned builder, then
    /// submit. A part is consumed on a successful submit (see [`SendOp`]).
    pub fn publish(&self, topic: &str) -> SendOp<Empty> {
        let topic = crate::operations::fixed_topic_or_panic(topic, "topic");
        crate::operations::socket_publish_op(crate::socket::pub_inner(self).handle, topic)
    }

    /// Registers a callback invoked when the socket can accept more sends after
    /// back-pressure. The callback runs on a background dispatch thread.
    pub fn on_send_ready<F>(&mut self, handler: F) -> Result<(), HandlerError>
    where
        F: Fn() + Send + 'static,
    {
        crate::socket::pub_inner_mut(self).on_send_ready(handler)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(crate::socket::pub_inner(self))
    }

    /// Returns the PUB-specific typed options facade.
    pub fn pub_options(&self) -> PubSocketOptions<'_> {
        PubSocketOptions::new(crate::socket::pub_inner(self))
    }
}

impl_pubsub_common!(
    PubSocket,
    crate::socket::pub_inner,
    crate::socket::pub_inner_mut
);

impl SubSocket {
    /// Receives the next matching topic message into caller-provided `out`
    /// storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn subscribe(&self, out: &mut TopicMessage, flags: RecvFlags) -> Result<bool, RecvError> {
        crate::socket::sub_inner(self).subscribe_recv(out, flags)
    }

    /// Adds a subscription for `filter` (an exact topic or pattern).
    /// Subscriptions accumulate; a socket may hold several at once.
    pub fn set_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        crate::socket::sub_inner(self).set_subscription(filter)
    }

    /// Removes a subscription previously added for `filter`.
    pub fn unset_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        crate::socket::sub_inner(self).unset_subscription(filter)
    }

    /// Returns the subscription filter and pattern flag at `index`, or `None`
    /// when no subscription exists at that index.
    pub fn subscription_at(&self, index: usize) -> Result<Option<(String, bool)>, ConfigError> {
        crate::socket::sub_inner(self).subscription_at(index)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(crate::socket::sub_inner(self))
    }

    /// Returns the SUB-specific typed options facade.
    pub fn sub_options(&self) -> SubSocketOptions<'_> {
        SubSocketOptions::new(crate::socket::sub_inner(self))
    }
}

impl_pubsub_common!(
    SubSocket,
    crate::socket::sub_inner,
    crate::socket::sub_inner_mut
);

impl XPubSocket {
    /// Begins publishing under `topic`: add parts on the returned builder, then
    /// submit. A part is consumed on a successful submit (see [`SendOp`]).
    pub fn publish(&self, topic: &str) -> SendOp<Empty> {
        let topic = crate::operations::fixed_topic_or_panic(topic, "topic");
        crate::operations::socket_publish_op(crate::socket::xpub_inner(self).handle, topic)
    }

    /// Receives the next subscriber (un)subscription event into caller-provided
    /// `out` storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no event is available.
    pub fn receive_subscription_event(
        &self,
        out: &mut SubscriptionEvent,
        flags: RecvFlags,
    ) -> Result<bool, RecvError> {
        crate::socket::xpub_inner(self).receive_subscription_event(out, flags)
    }

    /// Registers a callback invoked when the socket can accept more sends after
    /// back-pressure. The callback runs on a background dispatch thread.
    pub fn on_send_ready<F>(&mut self, handler: F) -> Result<(), HandlerError>
    where
        F: Fn() + Send + 'static,
    {
        crate::socket::xpub_inner_mut(self).on_send_ready(handler)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(crate::socket::xpub_inner(self))
    }

    /// Returns the PUB/XPUB-specific typed options facade.
    pub fn pub_options(&self) -> PubSocketOptions<'_> {
        PubSocketOptions::new(crate::socket::xpub_inner(self))
    }
}

impl_pubsub_common!(
    XPubSocket,
    crate::socket::xpub_inner,
    crate::socket::xpub_inner_mut
);

impl XSubSocket {
    /// Receives the next matching topic message into caller-provided `out`
    /// storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn subscribe(&self, out: &mut TopicMessage, flags: RecvFlags) -> Result<bool, RecvError> {
        crate::socket::xsub_inner(self).subscribe_recv(out, flags)
    }

    /// Adds a subscription for `filter` (an exact topic or pattern).
    /// Subscriptions accumulate; a socket may hold several at once.
    pub fn set_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        crate::socket::xsub_inner(self).set_subscription(filter)
    }

    /// Removes a subscription previously added for `filter`.
    pub fn unset_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        crate::socket::xsub_inner(self).unset_subscription(filter)
    }

    /// Returns the subscription filter and pattern flag at `index`, or `None`
    /// when no subscription exists at that index.
    pub fn subscription_at(&self, index: usize) -> Result<Option<(String, bool)>, ConfigError> {
        crate::socket::xsub_inner(self).subscription_at(index)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(crate::socket::xsub_inner(self))
    }

    /// Returns the SUB-specific typed options facade.
    pub fn sub_options(&self) -> SubSocketOptions<'_> {
        SubSocketOptions::new(crate::socket::xsub_inner(self))
    }
}

impl_pubsub_common!(
    XSubSocket,
    crate::socket::xsub_inner,
    crate::socket::xsub_inner_mut
);
