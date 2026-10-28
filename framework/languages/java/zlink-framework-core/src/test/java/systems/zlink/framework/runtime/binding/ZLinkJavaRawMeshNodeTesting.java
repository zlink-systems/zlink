package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

import java.util.Map;
import java.util.concurrent.Executor;

final class ZLinkJavaRawMeshNodeTesting {
    private ZLinkJavaRawMeshNodeTesting() {}

    static ZLinkJavaRawMeshNode create(
            String name,
            ZLinkJavaRawServicePort port,
            RouterSocket router,
            ZLinkServiceNodeDescriptor descriptor,
            ZLinkServiceTopologyRegistry topology,
            Map<RoutingId, Long> routes,
            Map<RoutingId, String> ready,
            Executor dispatch) {
        return new ZLinkJavaRawMeshNode(
                name,
                System::currentTimeMillis,
                System::nanoTime,
                ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
                ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT,
                port,
                dispatch,
                router,
                descriptor,
                topology,
                routes,
                ready);
    }
}
