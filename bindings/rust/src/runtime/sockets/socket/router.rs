use std::ffi::c_void;
use std::ptr;

use super::{SocketInner, close_unreceived_part};
use crate::core_context::Context;
use crate::domain::Received;
use crate::error::{ConfigError, RecvError};
use crate::ffi;
use crate::message::{Message, RoutingId};
use crate::native_errors::check_recv_rc;
use crate::socket_contracts::RouterSocket;

impl RouterSocket {
    pub(crate) fn new(ctx: &Context) -> Result<Self, ConfigError> {
        Ok(Self {
            inner: Box::new(SocketInner::create(
                ctx,
                ffi::zlink_socket_type_t::ZLINK_SOCKET_ROUTER,
            )?),
        })
    }
}

pub(crate) fn router_inner(socket: &RouterSocket) -> &SocketInner {
    &socket.inner
}

pub(crate) fn router_inner_mut(socket: &mut RouterSocket) -> &mut SocketInner {
    &mut socket.inner
}

pub(crate) fn recv_router_once(
    handle: *mut c_void,
    flags: u32,
    out: &mut Received,
) -> Result<bool, RecvError> {
    let mut source_rid = ptr::null();
    let mut request_seq = 0u64;
    let mut recv_flags = flags;
    let received = {
        let parts = out.receive_scratch();
        let mut received_any = false;

        loop {
            let mut part = std::mem::MaybeUninit::<ffi::zlink_msg_t>::uninit();
            unsafe {
                ffi::zlink_msg_init(part.as_mut_ptr());
            }
            let mut has_more = ffi::zlink_part_flag_t::ZLINK_PART_FINAL;
            let mut current_source_rid = ptr::null();
            let mut current_request_seq = 0u64;
            let rc = unsafe {
                ffi::zlink_router_recv_part(
                    handle,
                    &mut current_source_rid,
                    &mut current_request_seq,
                    part.as_mut_ptr(),
                    &mut has_more,
                    recv_flags,
                )
            };
            if !received_any {
                if rc == crate::error::RecvResult::NoData as i32 {
                    close_unreceived_part(&mut part);
                    break None;
                }
                if rc != 0 {
                    close_unreceived_part(&mut part);
                    let errno = unsafe { ffi::zlink_errno() };
                    if errno == libc::EAGAIN {
                        break None;
                    }
                    return Err(check_recv_rc(rc).unwrap_err());
                }
                source_rid = current_source_rid;
                request_seq = current_request_seq;
                parts.clear();
                received_any = true;
            } else if rc != 0 {
                close_unreceived_part(&mut part);
                return Err(check_recv_rc(rc).unwrap_err());
            }

            parts.push(unsafe { Message::from_raw(part.assume_init()) });
            if has_more == ffi::zlink_part_flag_t::ZLINK_PART_FINAL {
                let rid = if source_rid.is_null() {
                    RoutingId::from_raw(ffi::zlink_routing_id_t::empty())
                } else {
                    unsafe { RoutingId::from_raw(*source_rid) }
                };
                break Some((rid, request_seq));
            }
            recv_flags = ffi::ZLINK_DONTWAIT;
        }
    };

    if let Some((routing_id, request_seq)) = received {
        out.replace_router_parts(handle, routing_id, request_seq);
        Ok(true)
    } else {
        Ok(false)
    }
}
