package systems.zlink.framework.spring.internal.runtime;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import org.springframework.context.SmartLifecycle;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkFanoutPublishCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.host.ZLinkFrameworkRuntimeBootstrap;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;

public final class ZLinkFrameworkLifecycle
    implements SmartLifecycle, ZLinkClient, ZLinkFanoutClient, ZLinkRouteClient,
        ZLinkChannelRuntimeOptions,
        systems.zlink.framework.configuration.ZLinkMessageFlowControl {
    public static final int PHASE = 0;
    private static final Duration SPRING_SHUTDOWN_DRAIN_DEADLINE = Duration.ofSeconds(30);

    private final DefaultZLinkFrameworkOptions options;
    private final ZLinkBackendAdapterProvider backendAdapterFactory;
    private final ZLinkHandlerActivator handlerFactory;
    private final ZLinkRuntimeEventDispatcher eventDispatcher;
    private final systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
        routeMeshRuntime = new systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime() {
            @Override
            public systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot snapshot(
                String meshName) {
                return requireRuntime().routeMeshRuntime().snapshot(meshName);
            }

            @Override
            public java.util.concurrent.Flow.Publisher<
                systems.zlink.framework.monitoring.ZLinkObservedStatus<
                    systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot> > observe(
                    String meshName,
                    int capacity) {
                return requireRuntime().routeMeshRuntime().observe(meshName, capacity);
            }

            @Override
            public boolean isReady(String meshName) {
                return requireRuntime().routeMeshRuntime().isReady(meshName);
            }
        };
    private final systems.zlink.framework.monitoring.ZLinkClientServerRuntime
        clientServerRuntime =
            new systems.zlink.framework.monitoring.ZLinkClientServerRuntime() {
                @Override
                public systems.zlink.framework.monitoring.ZLinkClientServerStatus
                    snapshot(String channelName) {
                    return requireRuntime().clientServerRuntime().snapshot(channelName);
                }

                @Override
                public java.util.concurrent.Flow.Publisher<
                    systems.zlink.framework.monitoring.ZLinkObservedStatus<
                        systems.zlink.framework.monitoring.ZLinkClientServerStatus> > observe(
                        String channelName,
                        int capacity) {
                    return requireRuntime().clientServerRuntime().observe(
                        channelName, capacity);
                }

                @Override
                public boolean isReady(String channelName) {
                    return requireRuntime().clientServerRuntime().isReady(channelName);
                }
            };
    private final systems.zlink.framework.monitoring.ZLinkFanoutRuntime
        fanoutRuntime = new systems.zlink.framework.monitoring.ZLinkFanoutRuntime() {
            @Override
            public systems.zlink.framework.monitoring.ZLinkFanoutStatus snapshot(
                String channelName) {
                return requireRuntime().fanoutRuntime().snapshot(channelName);
            }

            @Override
            public java.util.concurrent.Flow.Publisher<
                systems.zlink.framework.monitoring.ZLinkObservedStatus<
                    systems.zlink.framework.monitoring.ZLinkFanoutStatus> > observe(
                    String channelName,
                    int capacity) {
                return requireRuntime().fanoutRuntime().observe(channelName, capacity);
            }
        };
    private ZLinkFrameworkRuntime runtime;
    private boolean running;

    public ZLinkFrameworkLifecycle(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendAdapterFactory,
        ZLinkHandlerActivator handlerFactory) {
        this.options = Objects.requireNonNull(options, "options");
        this.backendAdapterFactory = Objects.requireNonNull(
            backendAdapterFactory,
            "backendAdapterFactory");
        this.handlerFactory = Objects.requireNonNull(handlerFactory, "handlerFactory");
        this.eventDispatcher = new ZLinkRuntimeEventDispatcher();
    }

    @Override
    public synchronized void start() {
        if (running) {
            return;
        }
        runtime = ZLinkFrameworkRuntimeBootstrap.start(
            options,
            backendAdapterFactory,
            handlerFactory,
            eventDispatcher);
        running = true;
    }

    @Override
    public synchronized void stop() {
        if (!running) {
            return;
        }
        ZLinkFrameworkRuntime current = runtime;
        if (current == null) {
            running = false;
            return;
        }
        current.shutdown(SPRING_SHUTDOWN_DRAIN_DEADLINE).whenComplete((result, failure) -> {
            synchronized (ZLinkFrameworkLifecycle.this) {
                if (runtime == current) {
                    runtime = null;
                }
                running = false;
            }
        });
    }

    @Override
    public void stop(Runnable callback) {
        ZLinkFrameworkRuntime current;
        synchronized (this) {
            current = runtime;
            if (!running || current == null) {
                callback.run();
                return;
            }
        }
        // Spring shutdown must not start maintenance relocation.
        current.shutdown(SPRING_SHUTDOWN_DRAIN_DEADLINE).whenComplete((result, failure) -> {
            synchronized (ZLinkFrameworkLifecycle.this) {
                runtime = null;
                running = false;
            }
            callback.run();
        });
    }

    @Override
    public boolean isRunning() {
        return running;
    }

    @Override
    public boolean isAutoStartup() {
        return true;
    }

    @Override
    public int getPhase() {
        return PHASE;
    }

    @Override
    public void setMessageFlowMode(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode mode) {
        requireRuntime().setMessageFlowMode(mode);
    }

    @Override
    public systems.zlink.framework.configuration.ZLinkMessageFlowLogMode messageFlowMode() {
        return requireRuntime().messageFlowMode();
    }

    @Override
    public ZLinkSendCall sendToChannel(String channelName, Object message) {
        return requireRuntime().client().sendToChannel(channelName, message);
    }

    @Override
    public ZLinkRequestCall requestToChannel(String channelName, Object message) {
        return requireRuntime().client().requestToChannel(channelName, message);
    }

    public systems.zlink.framework.spots.SpotHandleResolver spotHandleResolver() {
        return requireRuntime().spotHandleResolver();
    }

    public systems.zlink.framework.spots.ActorSpotHandleResolver actorSpotHandleResolver() {
        return requireRuntime().actorSpotHandleResolver();
    }

    @Override
    public ZLinkFanoutPublishCall publish(
        String channelName,
        Object message) {
        return requireRuntime().fanout().publish(channelName, message);
    }

    @Override
    public ZLinkFanoutPublishCall publish(
        String channelName,
        String topic,
        Object message) {
        return requireRuntime().fanout().publish(channelName, topic, message);
    }

    @Override
    public ZLinkSendCall sendToNode(
        String channelName,
        RoutingId target,
        Object message) {
        return requireRuntime().route().sendToNode(channelName, target, message);
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotSendCall sendToSpot(
        String spotId,
        Object message) {
        return requireRuntime().route().sendToSpot(spotId, message);
    }

    @Override
    public ZLinkRequestCall requestToNode(
        String channelName,
        RoutingId target,
        Object message) {
        return requireRuntime().route().requestToNode(channelName, target, message);
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotRequestCall requestToSpot(
        String spotId,
        Object message) {
        return requireRuntime().route().requestToSpot(spotId, message);
    }

    public ZLinkSpotManager spotManager() {
        return requireRuntime().spotManager();
    }

    public ZLinkSpotOutbound spotOutbound() {
        return requireRuntime().spotOutbound();
    }

    public ZLinkSpotPublisherClient spotPublisherClient() {
        return requireRuntime().spotPublisherClient();
    }

    public ZLinkActorManager actorManager() {
        return requireRuntime().actorManager();
    }

    public ZLinkActorDirectory actorDirectory() {
        return requireRuntime().actorDirectory();
    }

    public ZLinkActorClient actorClient() {
        return requireRuntime().actorClient();
    }

    public systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
        routeMeshRuntime() {
        return routeMeshRuntime;
    }

    public systems.zlink.framework.monitoring.ZLinkClientServerRuntime
        clientServerRuntime() {
        return clientServerRuntime;
    }

    public systems.zlink.framework.monitoring.ZLinkFanoutRuntime
        fanoutRuntime() {
        return fanoutRuntime;
    }

    @Override
    public systems.zlink.framework.channels.ZLinkClientServerChannelRuntimeOptions clientServerChannel(
        String channelName) {
        return requireRuntime().channelRuntimeOptions().clientServerChannel(channelName);
    }

    @Override
    public systems.zlink.framework.channels.ZLinkRouteMeshChannelRuntimeOptions routeMeshChannel(
        String channelName) {
        return requireRuntime().channelRuntimeOptions().routeMeshChannel(channelName);
    }

    Map<String, systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode>
    monitoringMeshNodes() {
        return requireRuntime().meshNodesForInternalMonitoring();
    }

    systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions
    routeMeshRuntimeOptions() {
        return (systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions)
            requireRuntime().routeMeshRuntime();
    }

    public ZLinkLocationRuntimeQuery monitoringLocationRuntimeQuery() {
        return requireRuntime().monitoringLocationRuntimeQuery();
    }

    systems.zlink.framework.runtime.internal.monitoring
        .ZLinkMeshNodeMonitoringProjection monitoringMeshNodeProjection(
            String meshName,
            RoutingId rid) {
        return requireRuntime().monitoringMeshNodeProjection(meshName, rid);
    }

    java.util.List<String> monitoringMeshNodeChannelNames(String meshName) {
        return requireRuntime().monitoringMeshNodeChannelNames(meshName);
    }

    public boolean stopSpotRuntime() {
        return requireRuntime().stopSpotRuntime();
    }

    public java.util.concurrent.CompletionStage<ZLinkFrameworkRelocationResult> relocate(
        ZLinkFrameworkRelocationOptions options) {
        return requireRuntime().relocate(options);
    }

    public java.util.concurrent.CompletionStage<ZLinkFrameworkTerminationResult> shutdown() {
        return requireRuntime().shutdown();
    }

    public java.util.concurrent.CompletionStage<ZLinkFrameworkTerminationResult> shutdown(
        java.time.Duration deadline) {
        return requireRuntime().shutdown(deadline);
    }

    public boolean isReady() {
        return runtime != null && runtime.isReady();
    }

    public systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus status() {
        return requireRuntime().status();
    }

    public java.util.concurrent.Flow.Publisher<
        systems.zlink.framework.monitoring.ZLinkObservedStatus<
            systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus> > observe() {
        return requireRuntime().observe();
    }

    /**
     * Supplies the runtime instance for the lazy public Spring bean.
     * The bean is resolved after SmartLifecycle startup, so this method does
     * not start the runtime while the application context is being built.
     */
    public ZLinkFrameworkRuntime runtimeBean() {
        return requireRuntime();
    }

    private ZLinkFrameworkRuntime requireRuntime() {
        if (runtime == null) {
            throw new ZLinkConfigurationException("ZLink framework runtime is not running");
        }
        return runtime;
    }

}
