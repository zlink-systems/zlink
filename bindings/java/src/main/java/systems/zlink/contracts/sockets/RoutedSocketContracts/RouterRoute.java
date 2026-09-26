/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

/**
 * One row of a ROUTER selected-route snapshot: the peer routing id and the
 * generation of the route Core currently selects for it.
 *
 * @param routingId peer routing id
 * @param routeGeneration nonzero opaque equality token; it changes whenever
 *     Core selects a different route for the same routing id. Compare it only
 *     for equality (it is an unsigned 64-bit value; do not order values).
 */
public record RouterRoute(RoutingId routingId, long routeGeneration) {
    public RouterRoute {
        Objects.requireNonNull(routingId, "routingId");
    }
}
