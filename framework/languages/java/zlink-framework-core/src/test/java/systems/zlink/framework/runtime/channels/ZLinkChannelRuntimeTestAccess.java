package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledExecutorService;

public final class ZLinkChannelRuntimeTestAccess {
    private ZLinkChannelRuntimeTestAccess() {}

    public static ScheduledExecutorService timeoutExecutor(ZLinkChannelRuntime runtime) {
        return runtime.timeoutExecutor;
    }

    public static ExecutorService infrastructureExecutor(ZLinkChannelRuntime runtime) {
        return runtime.infrastructureExecutor;
    }

    public static ExecutorService replaceInfrastructureExecutor(
            ZLinkChannelRuntime runtime, ExecutorService executor) {
        var previous = runtime.infrastructureExecutor;
        runtime.infrastructureExecutor = executor;
        return previous;
    }

    public static SpotTransportAddressResolver replaceSpotAddressResolver(
            ZLinkChannelRuntime runtime, SpotTransportAddressResolver resolver) {
        var previous = runtime.spotAddressResolver;
        runtime.spotAddressResolver = resolver;
        return previous;
    }

    public static ZLinkBackendRouterSocket requireRouteRouter(
            ZLinkChannelRuntime runtime, String channelName) {
        return runtime.requireRouteRouter(channelName);
    }
}
