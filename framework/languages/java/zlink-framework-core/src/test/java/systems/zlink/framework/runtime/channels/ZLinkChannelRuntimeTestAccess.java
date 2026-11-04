package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;

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

    public static ZLinkBackendRouterSocket requireRouteRouter(
            ZLinkChannelRuntime runtime, String channelName) {
        return runtime.requireRouteRouter(channelName);
    }
}
