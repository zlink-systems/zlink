use crate::contracts::core::context::Context;
use crate::contracts::errors::errors::ConfigError;
use crate::contracts::sockets::pair_socket::PairSocket;
use crate::runtime::native::ffi;

use super::SocketInner;

impl PairSocket {
    pub(crate) fn new(ctx: &Context) -> Result<Self, ConfigError> {
        Ok(Self {
            inner: Box::new(SocketInner::create(
                ctx,
                ffi::zlink_socket_type_t::ZLINK_SOCKET_PAIR,
            )?),
        })
    }
}

pub(crate) fn pair_inner(socket: &PairSocket) -> &SocketInner {
    &socket.inner
}

pub(crate) fn pair_handle(socket: &PairSocket) -> *mut std::ffi::c_void {
    socket.inner.handle
}
