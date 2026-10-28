package systems.zlink.framework.runtime.internal.service;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.Map;

public final class ZLinkServiceTopologyRegistryTesting {
    private ZLinkServiceTopologyRegistryTesting() {}

    public static ZLinkServiceTopologyRegistry create(
            ZLinkServiceNodeDescriptor local,
            ZLinkStateLane lane,
            Map<RoutingId, String> readyConnections) {
        return new ZLinkServiceTopologyRegistry(local, lane, readyConnections);
    }
}
