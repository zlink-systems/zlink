use super::SocketInner;
use crate::core_context::Context;
use crate::error::ConfigError;
use crate::ffi;
use crate::socket_contracts::XSubSocket;

impl XSubSocket {
    pub(crate) fn new(ctx: &Context) -> Result<Self, ConfigError> {
        Ok(Self {
            inner: Box::new(SocketInner::create(
                ctx,
                ffi::zlink_socket_type_t::ZLINK_SOCKET_XSUB,
            )?),
        })
    }
}

pub(crate) fn xsub_inner(socket: &XSubSocket) -> &SocketInner {
    &socket.inner
}

pub(crate) fn xsub_inner_mut(socket: &mut XSubSocket) -> &mut SocketInner {
    &mut socket.inner
}
