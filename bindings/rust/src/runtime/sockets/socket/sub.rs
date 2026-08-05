use super::SocketInner;
use crate::core_context::Context;
use crate::error::ConfigError;
use crate::ffi;
use crate::socket_contracts::SubSocket;

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

pub(crate) fn sub_inner_mut(socket: &mut SubSocket) -> &mut SocketInner {
    &mut socket.inner
}
