use std::ptr;

use super::{SocketInner, recv_whole_message};
use crate::contracts::core::context::Context;
use crate::contracts::errors::errors::{ConfigError, RecvError};
use crate::contracts::messaging::message::RoutingId;
use crate::contracts::messaging::received::Received;
use crate::contracts::sockets::router_socket::RouterRoute;
use crate::contracts::sockets::router_socket::RouterSocket;
use crate::runtime::errors::native_errors::check_config_rc;
use crate::runtime::native::ffi;

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

impl SocketInner {
    pub(crate) fn recv_router_once(
        &self,
        flags: u32,
        out: &mut Received,
    ) -> Result<bool, RecvError> {
        let handle = self.handle;
        let routed = self.routed_handle.as_ref().expect("ROUTER handle").clone();
        let completion_owner = self
            .completion_owner
            .as_ref()
            .expect("ROUTER completion owner")
            .clone();
        let reply_owner = self
            .reply_owner
            .as_ref()
            .expect("ROUTER reply owner")
            .clone();
        let mut routing_id = RoutingId::from_raw(ffi::zlink_routing_id_t::empty());
        let mut reply_token = 0u64;
        let (parts, native_parts) = out.receive_scratch();
        let received = recv_whole_message(
            parts,
            native_parts,
            flags,
            |buffer, capacity, count, recv_flags| {
                let mut source_rid = ptr::null();
                let mut current_reply_token = 0u64;
                let rc = unsafe {
                    ffi::zlink_router_recv(
                        handle,
                        &mut source_rid,
                        &mut current_reply_token,
                        buffer,
                        capacity,
                        count,
                        recv_flags,
                    )
                };
                if rc == 0 {
                    if !source_rid.is_null() {
                        routing_id = unsafe { RoutingId::from_raw(*source_rid) };
                    }
                    reply_token = current_reply_token;
                }
                rc
            },
        )?;

        if received {
            // Core reports the route generation of the record returned by the
            // last successful zlink_router_recv; read it before any other data
            // receive.
            let route_generation = unsafe { ffi::zlink_router_recv_route_generation(handle) };
            out.replace_router_parts(
                handle,
                routed,
                completion_owner,
                reply_owner,
                routing_id,
                reply_token,
                route_generation,
            );
            Ok(true)
        } else {
            Ok(false)
        }
    }

    /// Reads the ROUTER selected-route snapshot. Core ROUTER §10.1:
    /// BUFFER_TOO_SMALL reports the required row count and keeps POLLROUTE
    /// readiness, so retry with that capacity.
    pub(crate) fn router_routes_snapshot(&self) -> Result<Vec<RouterRoute>, ConfigError> {
        let handle = self.handle;
        let mut capacity: usize = 16;
        loop {
            let mut native: Vec<ffi::zlink_router_route_t> = vec![
                ffi::zlink_router_route_t {
                    rid: ffi::zlink_routing_id_t::empty(),
                    route_generation: 0
                };
                capacity
            ];
            let mut count: usize = 0;
            let rc = unsafe {
                ffi::zlink_router_routes_snapshot(handle, native.as_mut_ptr(), capacity, &mut count)
            };
            if rc == ffi::zlink_config_result_t::ZLINK_CONFIG_BUFFER_TOO_SMALL as i32
                && count > capacity
            {
                // Core keeps POLLROUTE readiness on this result; retry with the
                // count it reported (the set can grow again before the retry).
                capacity = count;
                continue;
            }
            check_config_rc(rc)?;
            return Ok(native[..count]
                .iter()
                .map(|route| RouterRoute {
                    routing_id: RoutingId::from_raw(route.rid),
                    route_generation: route.route_generation,
                })
                .collect());
        }
    }
}
