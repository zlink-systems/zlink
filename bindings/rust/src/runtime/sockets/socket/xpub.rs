use crate::core_context::Context;
use crate::error::ConfigError;
use crate::ffi;
use crate::socket_contracts::XPubSocket;

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

pub(crate) fn xpub_inner_mut(socket: &mut XPubSocket) -> &mut SocketInner {
    &mut socket.inner
}
