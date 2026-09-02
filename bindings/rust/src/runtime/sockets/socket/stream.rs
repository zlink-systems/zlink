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
    let mut header = std::mem::MaybeUninit::<ffi::zlink_msg_t>::uninit();
    let mut body = std::mem::MaybeUninit::<ffi::zlink_msg_t>::uninit();
    unsafe {
        ffi::zlink_msg_init(header.as_mut_ptr());
        ffi::zlink_msg_init(body.as_mut_ptr());
    }
    let rc = unsafe {
        ffi::zlink_stream_recv_packet(
            handle,
            &mut rid,
            header.as_mut_ptr(),
            body.as_mut_ptr(),
            flags,
        )
    };
    if rc == RecvResult::NoData as i32 || (rc != 0 && unsafe { ffi::zlink_errno() } == libc::EAGAIN)
    {
        unsafe {
            ffi::zlink_msg_close(header.as_mut_ptr());
            ffi::zlink_msg_close(body.as_mut_ptr());
        }
        return Ok(false);
    }
    if rc != 0 {
        unsafe {
            ffi::zlink_msg_close(header.as_mut_ptr());
            ffi::zlink_msg_close(body.as_mut_ptr());
        }
        return Err(check_recv_rc(rc).expect_err("failed packet receive"));
    }
    if rid.is_null() {
        unsafe {
            ffi::zlink_msg_close(header.as_mut_ptr());
            ffi::zlink_msg_close(body.as_mut_ptr());
        }
        return Err(RecvError::new(RecvResult::InternalError, libc::EPROTO));
    }
    out.replace(
        unsafe { RoutingId::from_raw(*rid) },
        unsafe { Message::from_raw(header.assume_init()) },
        unsafe { Message::from_raw(body.assume_init()) },
    );
    Ok(true)
}
