package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

import java.util.Map;
import java.util.concurrent.Executor;
import java.util.function.BiFunction;

final class ZLinkJavaRawMeshNodeTesting {
    private ZLinkJavaRawMeshNodeTesting() {}

    static ZLinkJavaRawMeshNode create(
            String name,
            ZLinkJavaRawServicePort port,
            RouterSocket router,
            ZLinkServiceNodeDescriptor descriptor,
            ZLinkServiceTopologyRegistry topology,
            Map<RoutingId, Long> routes,
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
                routes);
    }

    static ZLinkJavaRawMeshNode create(
            Context context,
            String name,
            BiFunction<ZLinkServiceNodeDescriptor, ZLinkStateLane, ZLinkServiceTopologyRegistry>
                    topologyFactory) {
        return new ZLinkJavaRawMeshNode(
                name,
                System::currentTimeMillis,
                System::nanoTime,
                ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
                ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT,
                new ZLinkJavaRawServicePort(context),
                ZLinkProcessExecutionLanes.applicationLane(),
                null,
                null,
                null,
                Map.of(),
                topologyFactory);
    }
}
