// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;
use crate::{
    BindError, CommonSocketOptions, ConfigError, ConnectError, PubSocketOptions, RecvError,
    RecvFlags, SubSocketOptions, SubscriptionEvent, TopicMessage,
};
use crate::{Empty, PublishOp};

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

impl PubSocket {
    /// Begins publishing under `topic`: add parts on the returned builder, then
    /// submit. A part is consumed on a successful submit (see [`SendOp`]).
    pub fn publish(&self, topic: &str) -> PublishOp<Empty> {
        self.inner.socket_publish_op(topic)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(&self.inner)
    }

    /// Returns the PUB-specific typed options facade.
    pub fn pub_options(&self) -> PubSocketOptions<'_> {
        PubSocketOptions::new(&self.inner)
    }
}

impl_socket_common_base!(PubSocket);
impl_socket_connections!(PubSocket);

impl SubSocket {
    /// Receives the next matching topic message into caller-provided `out`
    /// storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn subscribe(&self, out: &mut TopicMessage, flags: RecvFlags) -> Result<bool, RecvError> {
        self.inner.subscribe_recv(out, flags)
    }

    /// Adds a subscription for `filter` (an exact topic or pattern).
    /// Subscriptions accumulate; a socket may hold several at once.
    pub fn set_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        self.inner.set_subscription(filter)
    }

    /// Removes a subscription previously added for `filter`.
    pub fn unset_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        self.inner.unset_subscription(filter)
    }

    /// Returns the subscription filter and pattern flag at `index`, or `None`
    /// when no subscription exists at that index.
    pub fn subscription_at(&self, index: usize) -> Result<Option<(String, bool)>, ConfigError> {
        self.inner.subscription_at(index)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(&self.inner)
    }

    /// Returns the SUB-specific typed options facade.
    pub fn sub_options(&self) -> SubSocketOptions<'_> {
        SubSocketOptions::new(&self.inner)
    }
}

impl_socket_common_base!(SubSocket);
impl_socket_connections!(SubSocket);

impl XPubSocket {
    /// Begins publishing under `topic`: add parts on the returned builder, then
    /// submit. A part is consumed on a successful submit (see [`SendOp`]).
    pub fn publish(&self, topic: &str) -> PublishOp<Empty> {
        self.inner.socket_publish_op(topic)
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
        self.inner.receive_subscription_event(out, flags)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(&self.inner)
    }

    /// Returns the PUB/XPUB-specific typed options facade.
    pub fn pub_options(&self) -> PubSocketOptions<'_> {
        PubSocketOptions::new(&self.inner)
    }
}

impl_socket_common_base!(XPubSocket);
impl_socket_connections!(XPubSocket);

impl XSubSocket {
    /// Receives the next matching topic message into caller-provided `out`
    /// storage.
    ///
    /// Returns `Ok(true)` on success and `Ok(false)` when
    /// [`RecvFlags::DONT_WAIT`] is set and no message is available.
    pub fn subscribe(&self, out: &mut TopicMessage, flags: RecvFlags) -> Result<bool, RecvError> {
        self.inner.subscribe_recv(out, flags)
    }

    /// Adds a subscription for `filter` (an exact topic or pattern).
    /// Subscriptions accumulate; a socket may hold several at once.
    pub fn set_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        self.inner.set_subscription(filter)
    }

    /// Removes a subscription previously added for `filter`.
    pub fn unset_subscription(&self, filter: &str) -> Result<(), ConfigError> {
        self.inner.unset_subscription(filter)
    }

    /// Returns the subscription filter and pattern flag at `index`, or `None`
    /// when no subscription exists at that index.
    pub fn subscription_at(&self, index: usize) -> Result<Option<(String, bool)>, ConfigError> {
        self.inner.subscription_at(index)
    }

    /// Returns the typed options facade common to all socket types.
    pub fn common_options(&self) -> CommonSocketOptions<'_> {
        CommonSocketOptions::new(&self.inner)
    }

    /// Returns the SUB-specific typed options facade.
    pub fn sub_options(&self) -> SubSocketOptions<'_> {
        SubSocketOptions::new(&self.inner)
    }
}

impl_socket_common_base!(XSubSocket);
impl_socket_connections!(XSubSocket);
