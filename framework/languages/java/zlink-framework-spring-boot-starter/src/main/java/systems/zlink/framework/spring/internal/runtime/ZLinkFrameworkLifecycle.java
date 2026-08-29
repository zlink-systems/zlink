package systems.zlink.framework.spring.internal.runtime;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Flow;
import systems.zlink.framework.channels.ZLinkClientServerChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkMessageFlowControl;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime;
import systems.zlink.framework.monitoring.ZLinkClientServerStatus;
import systems.zlink.framework.monitoring.ZLinkFanoutRuntime;
import systems.zlink.framework.monitoring.ZLinkFanoutStatus;
import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.spots.ActorSpotHandleResolver;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.spots.ZLinkSpotRequestCall;
import systems.zlink.framework.spots.ZLinkSpotSendCall;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
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
        ZLinkMessageFlowControl {
    public static final int PHASE = 0;
    private static final Duration SPRING_SHUTDOWN_DRAIN_DEADLINE = Duration.ofSeconds(30);
    private final DefaultZLinkFrameworkOptions options;
    private final ZLinkBackendAdapterProvider backendAdapterFactory;
    private final ZLinkHandlerActivator handlerFactory;
    private final ZLinkRuntimeEventDispatcher eventDispatcher;
    private final AtomicBoolean terminationLogged = new AtomicBoolean();
    private final ZLinkRouteMeshRuntime
        routeMeshRuntime = new ZLinkRouteMeshRuntime() {
            @Override
            public ZLinkMeshNodeSnapshot snapshot(
                String meshName) {
                return requireRuntime().routeMeshRuntime().snapshot(meshName);
            }

            @Override
            public Flow.Publisher<
                ZLinkObservedStatus<
                    ZLinkMeshNodeSnapshot> > observe(
                    String meshName,
                    int capacity) {
                return requireRuntime().routeMeshRuntime().observe(meshName, capacity);
            }

            @Override
            public boolean isReady(String meshName) {
                return requireRuntime().routeMeshRuntime().isReady(meshName);
            }
        };
    private final ZLinkClientServerRuntime
        clientServerRuntime =
            new ZLinkClientServerRuntime() {
                @Override
                public ZLinkClientServerStatus
                    snapshot(String channelName) {
                    return requireRuntime().clientServerRuntime().snapshot(channelName);
                }

                @Override
                public Flow.Publisher<
                    ZLinkObservedStatus<
                        ZLinkClientServerStatus> > observe(
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
    private final ZLinkFanoutRuntime
        fanoutRuntime = new ZLinkFanoutRuntime() {
            @Override
            public ZLinkFanoutStatus snapshot(
                String channelName) {
                return requireRuntime().fanoutRuntime().snapshot(channelName);
            }

            @Override
            public Flow.Publisher<
                ZLinkObservedStatus<
                    ZLinkFanoutStatus> > observe(
                    String channelName,
                    int capacity) {
                return requireRuntime().fanoutRuntime().observe(channelName, capacity);
            }
        };
    private ZLinkFrameworkRuntime runtime;
    private boolean running;
    private Thread processShutdownHook;

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
        terminationLogged.set(false);
        installProcessShutdownHook();
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
            logTerminationOnce(result, failure);
            synchronized (ZLinkFrameworkLifecycle.this) {
                if (runtime == current) {
                    runtime = null;
                }
                running = false;
                removeProcessShutdownHook();
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
            logTerminationOnce(result, failure);
            synchronized (ZLinkFrameworkLifecycle.this) {
                runtime = null;
                running = false;
                removeProcessShutdownHook();
            }
            callback.run();
        });
    }

    private void installProcessShutdownHook() {
        Thread hook = new Thread(this::shutdownFromProcessHook, "zlink-framework-shutdown");
        processShutdownHook = hook;
        try {
            Runtime.getRuntime().addShutdownHook(hook);
        } catch (IllegalStateException ignored) {
            processShutdownHook = null;
        }
    }

    private void removeProcessShutdownHook() {
        Thread hook = processShutdownHook;
        if (hook == null || hook == Thread.currentThread()) {
            return;
        }
        try {
            Runtime.getRuntime().removeShutdownHook(hook);
        } catch (IllegalStateException ignored) {
            // JVM shutdown is already in progress.
        }
        processShutdownHook = null;
    }

    private void shutdownFromProcessHook() {
        ZLinkFrameworkRuntime current;
        synchronized (this) {
            current = running ? runtime : null;
        }
        if (current == null) {
            return;
        }
        try {
            var result = current.shutdown(SPRING_SHUTDOWN_DRAIN_DEADLINE)
                .toCompletableFuture()
                .get(SPRING_SHUTDOWN_DRAIN_DEADLINE.toSeconds() + 5, TimeUnit.SECONDS);
            logTerminationOnce(result, null);
        } catch (Throwable failure) {
            logTerminationOnce(null, failure);
        }
    }

    private void logTerminationOnce(
        ZLinkFrameworkTerminationResult result,
        Throwable failure) {
        if (!terminationLogged.compareAndSet(false, true)) {
            return;
        }
        if (failure != null || result == null) {
            System.err.println(
                "ZLINK_FRAMEWORK_TERMINATION outcome=FORCE_STOPPED "
                    + "reason=TEARDOWN_FAILED");
            return;
        }
        System.err.println(
            "ZLINK_FRAMEWORK_TERMINATION outcome=" + result.outcome()
                + " reason=" + result.reason());
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
    public CompletableFuture<Void> setMessageFlowModeAsync(ZLinkMessageFlowLogMode mode) {
        return requireRuntime().setMessageFlowModeAsync(mode);
    }

    @Override
    public ZLinkMessageFlowLogMode messageFlowMode() {
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

    public SpotHandleResolver spotHandleResolver() {
        return requireRuntime().spotHandleResolver();
    }

    public ActorSpotHandleResolver actorSpotHandleResolver() {
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
    public ZLinkSpotSendCall sendToSpot(
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
    public ZLinkSpotRequestCall requestToSpot(
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

    public ZLinkRouteMeshRuntime
        routeMeshRuntime() {
        return routeMeshRuntime;
    }

    public ZLinkClientServerRuntime
        clientServerRuntime() {
        return clientServerRuntime;
    }

    public ZLinkFanoutRuntime
        fanoutRuntime() {
        return fanoutRuntime;
    }

    @Override
    public ZLinkClientServerChannelRuntimeOptions clientServerChannel(
        String channelName) {
        return requireRuntime().channelRuntimeOptions().clientServerChannel(channelName);
    }

    @Override
    public ZLinkRouteMeshChannelRuntimeOptions routeMeshChannel(
        String channelName) {
        return requireRuntime().channelRuntimeOptions().routeMeshChannel(channelName);
    }

    Map<String, ZLinkInternalMeshNode>
    monitoringMeshNodes() {
        return requireRuntime().meshNodesForInternalMonitoring();
    }

    ZLinkRouteMeshRuntimeOptions
    routeMeshRuntimeOptions() {
        return (ZLinkRouteMeshRuntimeOptions)
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

    List<String> monitoringMeshNodeChannelNames(String meshName) {
        return requireRuntime().monitoringMeshNodeChannelNames(meshName);
    }

    public boolean stopSpotRuntime() {
        return requireRuntime().stopSpotRuntime();
    }

    public CompletionStage<ZLinkFrameworkRelocationResult> relocate(
        ZLinkFrameworkRelocationOptions options) {
        return requireRuntime().relocate(options);
    }

    public CompletionStage<ZLinkFrameworkTerminationResult> shutdown() {
        return requireRuntime().shutdown();
    }

    public CompletionStage<ZLinkFrameworkTerminationResult> shutdown(
        Duration deadline) {
        return requireRuntime().shutdown(deadline);
    }

    public boolean isReady() {
        return runtime != null && runtime.isReady();
    }

    public ZLinkFrameworkRuntimeStatus status() {
        return requireRuntime().status();
    }

    public Flow.Publisher<
        ZLinkObservedStatus<
            ZLinkFrameworkRuntimeStatus> > observe() {
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
