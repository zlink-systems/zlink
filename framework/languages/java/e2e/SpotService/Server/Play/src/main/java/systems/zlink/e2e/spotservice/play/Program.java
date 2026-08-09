package systems.zlink.e2e.spotservice.play;
import com.fasterxml.jackson.databind.ObjectMapper;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import java.nio.file.Path;
import java.net.URI;
import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
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
import systems.zlink.e2e.spotservice.shared.CapacitySpot;
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
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    EvidenceHttpServer evidenceHttpServer(
        ScenarioState state,
        ObjectMapper json,
        ZLinkSpotManager spots,
        ZLinkActorManager actors,
        ZLinkActorClient actorClient,
        ZLinkRouteClient routes,
        ZLinkRouteMeshRuntime meshRuntime,
        ZLinkRouteMeshRuntimeOptions meshOptions,
        ObjectProvider<ZLinkFrameworkRuntime> frameworkRuntimes,
        PlayOptions options) {
        return new EvidenceHttpServer(
            state,
            json,
            options.httpEndpoint(),
            spots,
            actors,
            actorClient,
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
            installDispatchEvidence(state);
            options.addRelocationStore(relocationStore);
            options.configureLocations().setOwnerLeaseRenewInterval(
                Duration.ofMillis(play.locationHeartbeatMillis()));
            options.configureLocations().setOwnerLeaseTtl(
                Duration.ofMillis(play.locationLeaseTtlMillis()));
            options.addHandlersFromPackageOf(ActorAuthHandler.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL);
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
            // Spot callbacks use the same public ChannelName as a client.  The
            // process also owns the local server, so its client role loops back
            // to the advertised server endpoint.
            ingress.client().connect(play.ingressEndpoint());
            node.objects()
                .server()
                .addEntrySpot(ScenarioEntrySpot.class)
                .addSpotFactory(
                    "user",
                    UserSpot.class,
                    factory -> factory.recreateOnRelocation())
                .addSpotFactory(
                    "capacity",
                    CapacitySpot.class,
                    factory -> factory.stableTypeLimit(1).recreateOnRelocation())
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

    private static void installDispatchEvidence(ScenarioState state) {
        Logger.getLogger("systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer")
            .addHandler(new Handler() {
                @Override
                public void publish(LogRecord record) {
                    Map<String, String> fields = diagnosticsFields(record.getMessage());
                    if (fields != null && "ERROR".equals(fields.get("outcome"))) {
                        state.record(
                            "DispatchError",
                            fields.get("spot"),
                            fields.get("reason") + "/" + fields.get("action") + "/" + fields.get("packet"));
                    }
                }

                @Override public void flush() { }
                @Override public void close() { }
            });
    }

    private static Map<String, String> diagnosticsFields(String message) {
        if (message == null || !message.startsWith("message flow ")) {
            return null;
        }
        Map<String, String> fields = new HashMap<>();
        for (String field : message.substring("message flow ".length()).split(" ")) {
            String[] pair = field.split("=", 2);
            if (pair.length == 2) {
                fields.put(pair[0], pair[1]);
            }
        }
        return fields;
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
