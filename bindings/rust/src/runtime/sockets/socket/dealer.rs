use crate::contracts::core::context::Context;
use crate::contracts::errors::errors::ConfigError;
use crate::contracts::sockets::dealer_socket::DealerSocket;
use crate::runtime::native::ffi;

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
