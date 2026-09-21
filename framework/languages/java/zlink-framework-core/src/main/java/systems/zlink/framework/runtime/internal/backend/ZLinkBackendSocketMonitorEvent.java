package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;

import java.util.Optional;

public record ZLinkBackendSocketMonitorEvent(
        String event, Optional<RoutingId> routingId, String localAddress, String remoteAddress) {}
