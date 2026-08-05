use crate::core_context::Context;
use crate::error::ConfigError;
use crate::ffi;
use crate::socket_contracts::PubSocket;

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

pub(crate) fn pub_inner_mut(socket: &mut PubSocket) -> &mut SocketInner {
    &mut socket.inner
}
