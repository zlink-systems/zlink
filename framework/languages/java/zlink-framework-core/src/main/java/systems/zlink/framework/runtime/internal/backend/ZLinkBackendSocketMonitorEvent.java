package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;

import java.util.Optional;

public record ZLinkBackendSocketMonitorEvent(
        String event,
        Optional<RoutingId> routingId,
        String localAddress,
        String remoteAddress,
        int flags,
        long value) {
    public ZLinkBackendSocketMonitorEvent(
            String event,
            Optional<RoutingId> routingId,
            String localAddress,
            String remoteAddress,
            int flags) {
        this(event, routingId, localAddress, remoteAddress, flags, 0);
    }

    public ZLinkBackendSocketMonitorEvent(
            String event,
            Optional<RoutingId> routingId,
            String localAddress,
            String remoteAddress) {
        this(event, routingId, localAddress, remoteAddress, 0, 0);
    }
}
