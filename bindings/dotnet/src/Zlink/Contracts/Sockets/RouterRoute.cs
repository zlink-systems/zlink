// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     One row of a ROUTER selected-route snapshot: the routing id and the
///     generation of the route Core currently selects for it.
/// </summary>
/// <param name="RoutingId">Peer routing id.</param>
/// <param name="RouteGeneration">
///     Nonzero opaque equality token. It changes whenever Core selects a
///     different route for the same routing id; do not order values.
/// </param>
public readonly record struct RouterRoute(RoutingId RoutingId, ulong RouteGeneration);
