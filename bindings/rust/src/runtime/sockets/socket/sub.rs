use super::SocketInner;
use crate::contracts::core::context::Context;
use crate::contracts::errors::errors::ConfigError;
use crate::contracts::sockets::pubsub_sockets::SubSocket;
use crate::runtime::native::ffi;

impl SubSocket {
    pub(crate) fn new(ctx: &Context) -> Result<Self, ConfigError> {
        Ok(Self {
            inner: Box::new(SocketInner::create(
                ctx,
                ffi::zlink_socket_type_t::ZLINK_SOCKET_SUB,
            )?),
        })
    }
}

pub(crate) fn sub_inner(socket: &SubSocket) -> &SocketInner {
    &socket.inner
}
