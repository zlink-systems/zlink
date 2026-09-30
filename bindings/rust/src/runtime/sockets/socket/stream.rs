use std::ptr;

use crate::core_context::Context;
use crate::error::{ConfigError, RecvError, RecvResult};
use crate::ffi;
use crate::message::{Message, RoutingId};
use crate::native_errors::check_recv_rc;
use crate::stream_socket_contract::{StreamPacket, StreamSocket};

use super::SocketInner;

impl StreamSocket {
    pub(crate) fn new(ctx: &Context) -> Result<Self, ConfigError> {
        Ok(Self {
            inner: Box::new(SocketInner::create(
                ctx,
                ffi::zlink_socket_type_t::ZLINK_SOCKET_STREAM,
            )?),
        })
    }
}

pub(crate) fn stream_inner(socket: &StreamSocket) -> &SocketInner {
    &socket.inner
}
pub(crate) fn stream_inner_mut(socket: &mut StreamSocket) -> &mut SocketInner {
    &mut socket.inner
}

pub(crate) fn recv_stream_packet(
    handle: *mut std::ffi::c_void,
    out: &mut StreamPacket,
    flags: u32,
) -> Result<bool, RecvError> {
    out.reset();
    let mut rid = ptr::null();
    let mut header = Message::new()
        .map_err(|error| RecvError::new(RecvResult::InternalError, error.native_errno()))?;
    let mut body = Message::new()
        .map_err(|error| RecvError::new(RecvResult::InternalError, error.native_errno()))?;
    let rc = unsafe {
        ffi::zlink_stream_recv_packet(handle, &mut rid, header.raw_mut(), body.raw_mut(), flags)
    };
    if rc == RecvResult::NoData as i32 {
        return Ok(false);
    }
    if rc != 0 {
        return Err(check_recv_rc(rc).expect_err("failed packet receive"));
    }
    if rid.is_null() {
        return Err(RecvError::new(RecvResult::InternalError, libc::EPROTO));
    }
    out.replace(unsafe { RoutingId::from_raw(*rid) }, header, body);
    Ok(true)
}
