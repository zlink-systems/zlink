use std::ffi::c_void;
use std::ptr;

use super::{SocketInner, recv_part_sequence};
use crate::core_context::Context;
use crate::domain::Received;
use crate::error::{ConfigError, RecvError};
use crate::ffi;
use crate::message::RoutingId;
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
    routed: std::sync::Arc<crate::internal::RoutedHandle>,
    completion_owner: std::sync::Arc<crate::internal::CompletionOwner>,
    reply_owner: std::sync::Arc<crate::internal::RouterOwnerTag>,
    flags: u32,
    out: &mut Received,
) -> Result<bool, RecvError> {
    let mut routing_id = RoutingId::from_raw(ffi::zlink_routing_id_t::empty());
    let mut reply_token = 0u64;
    let received = recv_part_sequence(
        out.receive_scratch(),
        flags,
        |part, has_more, recv_flags, first| {
            let mut source_rid = ptr::null();
            let mut current_reply_token = 0u64;
            let rc = unsafe {
                ffi::zlink_router_recv_part(
                    handle,
                    &mut source_rid,
                    &mut current_reply_token,
                    part,
                    has_more,
                    recv_flags,
                )
            };
            if first && rc == 0 {
                if !source_rid.is_null() {
                    routing_id = unsafe { RoutingId::from_raw(*source_rid) };
                }
                reply_token = current_reply_token;
            }
            rc
        },
    )?;

    if received {
        out.replace_router_parts(
            handle,
            routed,
            completion_owner,
            reply_owner,
            routing_id,
            reply_token,
        );
        Ok(true)
    } else {
        Ok(false)
    }
}
