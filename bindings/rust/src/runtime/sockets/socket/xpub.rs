use crate::contracts::core::context::Context;
use crate::contracts::errors::errors::ConfigError;
use crate::contracts::sockets::pubsub_sockets::XPubSocket;
use crate::runtime::native::ffi;

use super::SocketInner;

impl XPubSocket {
    pub(crate) fn new(ctx: &Context) -> Result<Self, ConfigError> {
        Ok(Self {
            inner: Box::new(SocketInner::create(
                ctx,
                ffi::zlink_socket_type_t::ZLINK_SOCKET_XPUB,
            )?),
        })
    }
}

pub(crate) fn xpub_inner(socket: &XPubSocket) -> &SocketInner {
    &socket.inner
}
