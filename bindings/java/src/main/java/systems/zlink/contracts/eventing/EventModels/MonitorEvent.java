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
 * @param connectionId the process-local physical connection identity
 * @param transportLane the Core transport lane value
 * @param flags event-specific Core flags
 */
public record MonitorEvent(MonitorEventType event, long value,
                           Optional<RoutingId> routingId, String localAddr,
                           String remoteAddr, long connectionId,
                           int transportLane, int flags) {
    public MonitorEvent(MonitorEventType event, long value,
                        Optional<RoutingId> routingId, String localAddr,
                        String remoteAddr) {
        this(event, value, routingId, localAddr, remoteAddr,
             0L, 0, 0);
    }

    public MonitorEvent {
        routingId = routingId == null ? Optional.empty() : routingId;
    }

}
