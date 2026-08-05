package systems.zlink.e2e.runtimemonitoring.service;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import java.nio.file.Path;
import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.e2e.runtimemonitoring.service.handlers.MonitoringSpot;
import systems.zlink.e2e.runtimemonitoring.service.handlers.TriggeredMonitoringSpot;
import systems.zlink.e2e.runtimemonitoring.service.handlers.WorkReqHandler;
import systems.zlink.e2e.runtimemonitoring.service.support.EvidenceHttpServer;
import systems.zlink.e2e.runtimemonitoring.service.support.EvidenceState;
import systems.zlink.e2e.runtimemonitoring.service.support.ObserverIsolationProbe;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;

@EnableZLinkFramework
@EnableConfigurationProperties(ServiceOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.runtimemonitoring.service")
public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        run(args);
    }

    public static void run(String... args) {
        String config = configPath(args);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(isolatedEnvironment())
            .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        builder.run();
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    EvidenceState evidenceState(ServiceOptions config) {
        return new EvidenceState(config.routingId());
    }

    @Bean
    EvidenceHttpServer evidenceHttpServer(
        EvidenceState state,
        ObjectMapper json,
        systems.zlink.framework.channels.ZLinkChannelRuntimeOptions runtimeOptions,
        systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions meshRuntimeOptions,
        systems.zlink.framework.channels.ZLinkRouteClient routeClient,
        ObjectProvider<ZLinkRouteMeshRuntime> meshRuntime,
        ObjectProvider<ZLinkFrameworkLifecycle> runtimeQuery,
        ObserverIsolationProbe observerIsolation,
        ObjectProvider<ZLinkSpotManager> spots,
        ObjectProvider<ZLinkSpotPublisherClient> publisher,
        org.springframework.context.ConfigurableApplicationContext applicationContext,
        ServiceOptions config) {
        return new EvidenceHttpServer(
            state,
            json,
            runtimeOptions,
            meshRuntimeOptions,
            routeClient,
            meshRuntime,
            runtimeQuery,
            observerIsolation,
            spots,
            publisher,
            applicationContext,
            config.httpEndpoint());
    }

    @Bean
    ZLinkFrameworkConfigurer frameworkConfigurer(ServiceOptions config) {
        return options -> {
            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(500));
            options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(3));
            options.configureLocations().setPollingInterval(Duration.ofMillis(250));
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(config.logDirectory() + "/service-flow.log")
                .traceLabel("java-mon-service");
            java.net.URI apiEndpoint = java.net.URI.create(config.apiEndpoint());
            options.addClientServerChannel(Contracts.CHANNEL)
                .server()
                .setAdvertiseHost(apiEndpoint.getHost())
                .listen(apiEndpoint.getPort())
                .addRequestHandler(
                    WorkReqHandler.class,
                    Contracts.WorkReq.class,
                    Contracts.WorkRes.class);
            if (config.enableHandshake()) {
                java.net.URI handshakeEndpoint =
                    java.net.URI.create(config.handshakeEndpoint());
                options.addClientServerChannel(Contracts.HANDSHAKE_CHANNEL)
                    .server()
                    .setAdvertiseHost(handshakeEndpoint.getHost())
                    .listen(handshakeEndpoint.getPort())
                    .addRequestHandler(
                        WorkReqHandler.class,
                        Contracts.WorkReq.class,
                        Contracts.WorkRes.class);
            }
            if (config.enableSpot()) {
                ZLinkMeshNodeBuilder node = options.addRouteMesh(Contracts.SPOT_MESH)
                    .listen(config.meshEndpoint())
                    .setRoutingIdPrefix(config.routingId());
                node.configureRouterSocket().setReceiveHighWaterMark(1);
                node.channelName(Contracts.SPOT_CHANNEL)
                    .server()
                    .addRequestHandler(
                        WorkReqHandler.class,
                        Contracts.WorkReq.class,
                        Contracts.WorkRes.class);
                if (!config.meshPeerEndpoint().isBlank()) {
                    node.peerConnections().connect(config.meshPeerEndpoint());
                }
                var objects = node.objects().server();
                objects.addSpotFactory(
                    Contracts.MONITORING_SPOT_TYPE,
                    MonitoringSpot.class,
                    factory -> factory.disableRelocation());
                objects.addSpotFactory(
                    Contracts.TRIGGERED_MONITORING_SPOT_TYPE,
                    TriggeredMonitoringSpot.class,
                    factory -> factory.disableRelocation());
            }
        };
    }

    @Bean
    WorkReqHandler workRequestHandler(EvidenceState state) {
        return new WorkReqHandler(state);
    }

    @Bean
    ZLinkRedisLocationStore locationStore(ServiceOptions config) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(config.redisLocationEndpoint())
            .setKeyPrefix(config.locationKeyPrefix())
            .setCommandTimeout(Duration.ofMillis(500)));
    }

    @Bean
    ApplicationRunner createSpot(ObjectProvider<ZLinkSpotManager> spots, ServiceOptions config) {
        return ignored -> {
            if (!config.enableSpot()) {
                return;
            }
            ZLinkSpotManager manager = spots.getIfAvailable();
            if (manager == null) {
                throw new IllegalStateException("spot manager is required when spot monitoring is enabled");
            }
            manager.getOrCreate(
                    "monitoring-room-" + config.routingId(),
                    Contracts.MONITORING_SPOT_TYPE)
                .request(ZLinkMessage.of("bootstrap"))
                .submit()
                .whenComplete((ignoredResult, error) -> {
                    if (error != null) {
                        throw new IllegalStateException("failed to create monitoring Spot", error);
                    }
                });
        };
    }

    @Bean
    ApplicationRunner recordMonitoringLifecycle(
        org.springframework.beans.factory.ObjectProvider<ZLinkFrameworkLifecycle> lifecycle,
        EvidenceState state) {
        return ignored -> state.record(
            "system",
            "service",
            "FrameworkLifecycle",
            "running=" + lifecycle.stream().anyMatch(ZLinkFrameworkLifecycle::isRunning));
    }

    @Bean
    ApplicationRunner recordRouteMeshRuntimeEvents(
        ObjectProvider<ZLinkRouteMeshRuntime> runtimeProvider,
        EvidenceState state,
        ServiceOptions config) {
        return ignored -> {
            if (!config.enableSpot()) {
                return;
            }
            ZLinkRouteMeshRuntime runtime = runtimeProvider.getIfAvailable();
            if (runtime == null) {
                throw new IllegalStateException("RouteMesh runtime is required");
            }
            runtime.observe(Contracts.SPOT_MESH, 32).subscribe(
                new java.util.concurrent.Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>() {
                    @Override
                    public void onSubscribe(java.util.concurrent.Flow.Subscription subscription) {
                        subscription.request(Long.MAX_VALUE);
                    }

                    @Override
                    public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> observed) {
                        ZLinkMeshNodeSnapshot status = observed.status();
                        state.record(
                            "route-mesh-runtime",
                            status.meshName(),
                            "status-changed",
                            "sequence=" + status.sequence()
                                + "|state=" + status.state()
                                + "|readyPeers=" + status.readyPeerCount());
                    }

                    @Override
                    public void onError(Throwable error) {
                        state.record(
                            "route-mesh-runtime",
                            Contracts.SPOT_MESH,
                            "observer-error",
                            error.getClass().getName() + ": " + error.getMessage());
                    }

                    @Override
                    public void onComplete() {
                    }
                });
        };
    }

    @Bean
    ObserverIsolationProbe observerIsolationProbe() {
        return new ObserverIsolationProbe();
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: runtime-monitoring-service --config <path>");
        }
        return args[1];
    }

    private static StandardEnvironment isolatedEnvironment() {
        StandardEnvironment value = new StandardEnvironment();
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        value.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        return value;
    }
}
