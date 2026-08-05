package systems.zlink.e2e.spotservice.play;

import java.nio.file.Path;
import java.net.URI;
import java.time.Duration;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.spotservice.shared.ActorAuthHandler;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.e2e.spotservice.shared.EvidenceHttpServer;
import systems.zlink.e2e.spotservice.shared.IngressMsgHandler;
import systems.zlink.e2e.spotservice.shared.MismatchedSpot;
import systems.zlink.e2e.spotservice.shared.MultiBindHandler;
import systems.zlink.e2e.spotservice.shared.NoopIngressHandler;
import systems.zlink.e2e.spotservice.shared.RouteReqHandler;
import systems.zlink.e2e.spotservice.shared.ScenarioActorFactory;
import systems.zlink.e2e.spotservice.shared.ScenarioActor;
import systems.zlink.e2e.spotservice.shared.ScenarioEntrySpot;
import systems.zlink.e2e.spotservice.shared.ScenarioSession;
import systems.zlink.e2e.spotservice.shared.ScenarioState;
import systems.zlink.e2e.spotservice.shared.SlowSessionHandler;
import systems.zlink.e2e.spotservice.shared.TimerScenarioSpot;
import systems.zlink.e2e.spotservice.shared.UserSpot;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ClientServerChannelBuilder;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spots.ZLinkSpotManager;

@EnableZLinkFramework
@EnableConfigurationProperties(PlayOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.spotservice.play")
public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        String config = configPath(args);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(isolatedEnvironment())
            .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        builder.run();
    }

    @Bean
    ScenarioState scenarioState(PlayOptions options) {
        return new ScenarioState(options.nodeRid());
    }

    @Bean
    com.fasterxml.jackson.databind.ObjectMapper objectMapper() {
        return new com.fasterxml.jackson.databind.ObjectMapper();
    }

    @Bean
    EvidenceHttpServer evidenceHttpServer(
        ScenarioState state,
        com.fasterxml.jackson.databind.ObjectMapper json,
        ZLinkSpotManager spots,
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime meshRuntime,
        systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions meshOptions,
        ObjectProvider<systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime> frameworkRuntimes,
        PlayOptions options) {
        return new EvidenceHttpServer(
            state,
            json,
            options.httpEndpoint(),
            spots,
            routes,
            meshRuntime,
            meshOptions,
            frameworkRuntimes);
    }

    @Bean
    ZLinkFrameworkConfigurer playFramework(
        ScenarioState state,
        PlayOptions play,
        ZLinkRedisRelocationStore relocationStore) {
        return options -> {
            String nodeRid = state.nodeRid();
            String logDir = play.logDir();
            options.addRelocationStore(relocationStore);
            options.configureLocations().setOwnerLeaseRenewInterval(
                Duration.ofMillis(play.locationHeartbeatMillis()));
            options.configureLocations().setOwnerLeaseTtl(
                Duration.ofMillis(play.locationLeaseTtlMillis()));
            options.addHandlersFromPackageOf(ActorAuthHandler.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/" + nodeRid + "-flow.log")
                .traceLabel("java-sm-" + nodeRid)
                .setMessageFlowObserver(error -> {
                    if (error.outcome() != ZLinkMessageFlowOutcome.ERROR) {
                        return java.util.concurrent.CompletableFuture.completedFuture(null);
                    }
                    state.record(
                        "DispatchError",
                        error.spotId(),
                        error.errorReason() + "/" + error.errorAction() + "/" + error.packetName());
                    return java.util.concurrent.CompletableFuture.completedFuture(null);
                });
            ZLinkMeshNodeBuilder node = options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(play.routeEndpoint())
                .setRoutingId(RoutingId.from(nodeRid));
            node.channelName(Contracts.ROUTE_CHANNEL).server();
            node.addRouteRequestHandler(
                RouteReqHandler.class,
                Contracts.RouteReq.class,
                Contracts.RouteRes.class);
            ClientServerChannelBuilder ingress = options.addClientServerChannel(Contracts.INGRESS_CHANNEL);
            var ingressServer = ingress.server()
                .listen(URI.create(play.ingressEndpoint()).getPort());
            ingressServer.addSendHandler(
                IngressMsgHandler.class,
                Contracts.OutboundMsg.class);
            ingressServer.addRequestHandler(
                NoopIngressHandler.class,
                Contracts.StateReq.class,
                String.class);
            node.objects()
                .server()
                .addEntrySpot(ScenarioEntrySpot.class)
                .addSpotFactory(
                    "user",
                    UserSpot.class,
                    factory -> factory.recreateOnRelocation())
                .addSpotFactory(
                    "mismatched",
                    MismatchedSpot.class,
                    factory -> factory.disableRelocation())
                .addSpotFactory(
                    "timer",
                    TimerScenarioSpot.class,
                    factory -> factory.disableRelocation())
                .addActorFactory(
                    "scenario",
                    ScenarioActor.class,
                    ScenarioActorFactory.class,
                    factory -> factory.recreateOnRelocation());
            String streamEndpoint = play.streamEndpoint();
            String tlsStreamEndpoint = play.tlsStreamEndpoint();
            if (!streamEndpoint.isBlank() || !tlsStreamEndpoint.isBlank()) {
                var stream = options.addStreamNode("gateway");
                if (!streamEndpoint.isBlank()) {
                    stream.bind(streamEndpoint);
                }
                if (!tlsStreamEndpoint.isBlank()) {
                    stream.bind(tlsStreamEndpoint)
                        .setTlsServer(
                            play.tlsCertificatePath(),
                            play.tlsKeyPath());
                }
                stream.enableActorDispatch();
                stream.registerSession(ScenarioSession.class);
            }
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(PlayOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    @Bean
    ZLinkRedisRelocationStore relocationStore(PlayOptions options) {
        return new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: spot-service-play --config <path>");
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
