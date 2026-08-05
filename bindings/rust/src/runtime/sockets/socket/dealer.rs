use crate::core_context::Context;
use crate::error::ConfigError;
use crate::ffi;
use crate::socket_contracts::DealerSocket;

use super::SocketInner;

impl DealerSocket {
    pub(crate) fn new(ctx: &Context) -> Result<Self, ConfigError> {
        Ok(Self {
            inner: Box::new(SocketInner::create(
                ctx,
                ffi::zlink_socket_type_t::ZLINK_SOCKET_DEALER,
            )?),
        })
    }
}

pub(crate) fn dealer_inner(socket: &DealerSocket) -> &SocketInner {
    &socket.inner
}

pub(crate) fn dealer_inner_mut(socket: &mut DealerSocket) -> &mut SocketInner {
    &mut socket.inner
}
