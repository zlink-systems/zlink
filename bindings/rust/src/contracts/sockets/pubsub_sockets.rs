// SPDX-License-Identifier: MPL-2.0

use crate::internal::SocketStorage;

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
