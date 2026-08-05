/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

import systems.zlink.contracts.core.RoutingId;
import java.util.Optional;

/**
 * A single socket connection-lifecycle event reported by a monitor.
 * @param event the kind of event
 * @param value the event value (e.g., fd or error code)
 * @param routingId the peer routing id, if delivered with the event
 * @param localAddr the local address, if delivered with the event
 * @param remoteAddr the remote address, if delivered with the event
 */
public record MonitorEvent(MonitorEventType event, long value,
                           Optional<RoutingId> routingId, String localAddr,
                           String remoteAddr) {
    public MonitorEvent {
        routingId = routingId == null ? Optional.empty() : routingId;
    }

}
