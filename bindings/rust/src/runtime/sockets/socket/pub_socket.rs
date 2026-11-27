use crate::contracts::core::context::Context;
use crate::contracts::errors::errors::ConfigError;
use crate::contracts::sockets::pubsub_sockets::PubSocket;
use crate::runtime::native::ffi;

use super::SocketInner;

impl PubSocket {
    pub(crate) fn new(ctx: &Context) -> Result<Self, ConfigError> {
        Ok(Self {
            inner: Box::new(SocketInner::create(
                ctx,
                ffi::zlink_socket_type_t::ZLINK_SOCKET_PUB,
            )?),
        })
    }
}

pub(crate) fn pub_inner(socket: &PubSocket) -> &SocketInner {
    &socket.inner
}
