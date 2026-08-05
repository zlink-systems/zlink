use crate::core_context::Context;
use crate::error::ConfigError;
use crate::ffi;
use crate::socket_contracts::PairSocket;

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

pub(crate) fn pair_inner_mut(socket: &mut PairSocket) -> &mut SocketInner {
    &mut socket.inner
}

pub(crate) fn pair_handle(socket: &PairSocket) -> *mut std::ffi::c_void {
    socket.inner.handle
}
