package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.service.ZLinkClassicFanoutLiveness;

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

    public static ZLinkChannelRuntime withInfrastructureExecutor(
            ZLinkChannelBackendAdapter backend,
            ZLinkBackendContext context,
            ZLinkBackendAdapterProvider backendFactory,
            ZLinkBackendAdapterOptions adapterOptions,
            ZLinkFrameworkRegistration registration,
            ZLinkMessageSerializer serializer,
            ZLinkHandlerActivator handlerFactory,
            ZLinkRuntimeEventDispatcher eventDispatcher,
            ExecutorService infrastructureExecutor) {
        return new ZLinkChannelRuntime(
                backend,
                context,
                backendFactory,
                adapterOptions,
                registration,
                serializer,
                handlerFactory,
                eventDispatcher,
                infrastructureExecutor);
    }

    public static ZLinkBackendRouterSocket requireRouteRouter(
            ZLinkChannelRuntime runtime, String channelName) {
        return runtime.requireRouteRouter(channelName);
    }

    public static void awaitManualFanoutPublisher(ZLinkChannelRuntime runtime, String channelName)
            throws InterruptedException {
        var manualFanout = runtime.monitoringManualFanout();
        long deadline =
                System.nanoTime() + ZLinkClassicFanoutLiveness.DEFAULT_PUBLISHER_TIMEOUT.toNanos();
        do {
            if (manualFanout.publisherSnapshots(channelName).stream()
                    .anyMatch(ZLinkManualFanoutRuntime.PublisherSnapshot::ready)) {
                return;
            }
            Thread.sleep(10);
        } while (System.nanoTime() < deadline);
        throw new AssertionError("No ready manual fanout publisher for " + channelName);
    }
}
