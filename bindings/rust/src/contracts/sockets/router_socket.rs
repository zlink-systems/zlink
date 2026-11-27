// SPDX-License-Identifier: MPL-2.0

use crate::RoutingId;
use crate::internal::SocketStorage;

/// One row of a ROUTER selected-route snapshot: the peer routing id and the
/// generation of the route Core currently selects for it.
///
/// `route_generation` is a nonzero opaque equality token; it changes whenever
/// Core selects a different route for the same routing id. Compare it only
/// for equality.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct RouterRoute {
    pub routing_id: RoutingId,
    pub route_generation: u64,
}

/// ROUTER socket: routes messages to peers addressed by routing id, the
/// server side of asynchronous request/reply.
pub struct RouterSocket {
    pub(crate) inner: Box<SocketStorage>,
}

impl std::panic::UnwindSafe for RouterSocket {}
impl std::panic::RefUnwindSafe for RouterSocket {}
