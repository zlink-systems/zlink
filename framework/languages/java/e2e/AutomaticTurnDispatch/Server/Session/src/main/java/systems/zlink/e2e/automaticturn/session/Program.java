package systems.zlink.e2e.automaticturn.session;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.automaticturn.shared.BindActorsHandler;
import systems.zlink.e2e.automaticturn.shared.Contracts;
import systems.zlink.e2e.automaticturn.shared.EnsureSpotHandler;
import systems.zlink.e2e.automaticturn.shared.EvidenceHttpServer;
import systems.zlink.e2e.automaticturn.shared.EvidenceStore;
import systems.zlink.e2e.automaticturn.shared.ScenarioReqHandler;
import systems.zlink.e2e.automaticturn.shared.ShutdownAwaitSessionHandlers;
import systems.zlink.e2e.automaticturn.shared.SpotCommandHandler;
import systems.zlink.e2e.automaticturn.shared.RemoteSpotAwaitSessionHandler;
import systems.zlink.e2e.automaticturn.shared.AwaitActorFactory;
import systems.zlink.e2e.automaticturn.shared.AwaitActor;
import systems.zlink.e2e.automaticturn.shared.AwaitEntrySpot;
import systems.zlink.e2e.automaticturn.shared.AwaitProbeHandlers;
import systems.zlink.e2e.automaticturn.shared.AwaitProbeSpot;
import systems.zlink.e2e.automaticturn.shared.AwaitSession;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spots.SpotHandleResolver;
import io.micrometer.core.instrument.MeterRegistry;
import systems.zlink.e2e.automaticturn.shared.DrainEvidence;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;
import org.springframework.boot.ApplicationRunner;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.messaging.ZLinkMessage;

@EnableZLinkFramework
@EnableConfigurationProperties(SessionOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.automaticturn.session")
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
    EvidenceStore evidenceStore() {
        return new EvidenceStore("session-a");
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    EvidenceHttpServer evidenceHttpServer(
        EvidenceStore evidence,
        ObjectMapper json,
        MeterRegistry metrics,
        ZLinkFrameworkLifecycle lifecycle,
        DrainEvidence drainEvidence,
        SessionOptions config) {
        drainEvidence.observe(lifecycle.observe());
        return new EvidenceHttpServer(
            evidence, json, config.httpEndpoint(), metrics,
            lifecycle, lifecycle::monitoringLocationRuntimeQuery, drainEvidence, null, null);
    }

    @Bean
    DrainEvidence drainEvidence() { return new DrainEvidence(); }

    @Bean(destroyMethod = "close")
    systems.zlink.e2e.automaticturn.shared.PersistentRoomEvents persistentRoomEvents(SessionOptions config) {
        return new systems.zlink.e2e.automaticturn.shared.PersistentRoomEvents(
            config.redisLocationEndpoint(), config.locationKeyPrefix());
    }

    @Bean
    ZLinkFrameworkConfigurer framework(SessionOptions config) {
        return options -> {
            options.addHandlersFromPackageOf(ScenarioReqHandler.class);
            var dispatch = options.configureDispatch()
                .messageFlow("off".equals(config.messageFlowMode())
                    ? ZLinkMessageFlowLogMode.OFF
                    : ZLinkMessageFlowLogMode.KEY_TRANSITIONS);
            if (!"off".equals(config.messageFlowMode())) {
                dispatch.traceLogFile(config.logDirectory() + "/session-flow.log")
                    .traceLabel("java-atd-session");
            }
            ZLinkMeshNodeBuilder mesh = options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(config.sessionRouteEndpoint())
                .setRoutingId(RoutingId.from("session-a"));
            mesh.channelName(Contracts.ROUTE_CHANNEL);
            mesh.peerConnections().connect(config.routeEndpoint());
            String routeBEndpoint = config.routeBEndpoint();
            if (!routeBEndpoint.isBlank()) {
                mesh.peerConnections().connect(routeBEndpoint);
            }
            options.addClientServerChannel(Contracts.DELAY_CHANNEL)
                .enableClient(config.delayEndpoint());
            mesh.objects()
                .server()
                .addEntrySpot(AwaitEntrySpot.class)
                .addSpotFactory(
                    "await-probe",
                    AwaitProbeSpot.class,
                    factory -> factory.disableRelocation())
                .addActorFactory(
                    Contracts.ACTOR_TYPE,
                    AwaitActor.class,
                    AwaitActorFactory.class,
                    factory -> factory.recreateOnRelocation());
            options.addStreamNode("session")
                .bind(config.streamEndpoint())
                .registerSession(AwaitSession.class);
        };
    }

    @Bean
    ApplicationRunner createDrainSpot(ZLinkSpotManager spots, SessionOptions config) {
        return ignored -> {
            String spotRid = config.sessionDrainSpotRid();
            if (!spotRid.isBlank()) {
                spots.getOrCreate(
                    AwaitProbeSpot.class,
                    RoutingId.from(spotRid),
                    ZLinkMessage.of("drain-hold")).toCompletableFuture().join();
            }
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(SessionOptions config) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(config.redisLocationEndpoint())
            .setKeyPrefix(config.locationKeyPrefix()));
    }

    @Bean
    ScenarioReqHandler scenarioRequestHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots,
        EvidenceStore evidence) {
        return new ScenarioReqHandler(routes, spots, evidence);
    }

    @Bean
    ShutdownAwaitSessionHandlers.Wait shutdownAwaitWaitHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new ShutdownAwaitSessionHandlers.Wait(routes, spots);
    }

    @Bean
    ShutdownAwaitSessionHandlers.Recovery shutdownAwaitRecoveryHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new ShutdownAwaitSessionHandlers.Recovery(routes, spots);
    }

    @Bean
    BindActorsHandler bindActorsHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        EvidenceStore evidence) {
        return new BindActorsHandler(routes, evidence);
    }

    @Bean
    EnsureSpotHandler ensureSpotHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes) {
        return new EnsureSpotHandler(routes);
    }

    @Bean
    systems.zlink.e2e.automaticturn.shared.PersistentRoomStateSessionHandler persistentRoomStateSessionHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        systems.zlink.framework.spots.SpotHandleResolver spots) {
        return new systems.zlink.e2e.automaticturn.shared.PersistentRoomStateSessionHandler(
            routes, spots);
    }

    @Bean
    RemoteSpotAwaitSessionHandler remoteSpotAwaitSessionHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new RemoteSpotAwaitSessionHandler(routes, spots);
    }

    @Bean
    SpotCommandHandler.WorkerAwait workerAwaitMsgHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new SpotCommandHandler.WorkerAwait(routes, spots);
    }

    @Bean
    SpotCommandHandler.Await awaitCommandHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new SpotCommandHandler.Await(routes, spots);
    }

    @Bean
    SpotCommandHandler.AwaitTimeout awaitTimeoutCommandHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new SpotCommandHandler.AwaitTimeout(routes, spots);
    }

    @Bean
    SpotCommandHandler.AwaitCancel awaitCancelCommandHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new SpotCommandHandler.AwaitCancel(routes, spots);
    }

    @Bean
    SpotCommandHandler.Probe probeCommandHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new SpotCommandHandler.Probe(routes, spots);
    }

    @Bean
    SpotCommandHandler.ProbeRequest probeRequestHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new SpotCommandHandler.ProbeRequest(routes, spots);
    }

    @Bean
    SpotCommandHandler.TimerStart timerStartCommandHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new SpotCommandHandler.TimerStart(routes, spots);
    }

    @Bean
    SpotCommandHandler.TimerStop timerStopCommandHandler(
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        SpotHandleResolver spots) {
        return new SpotCommandHandler.TimerStop(routes, spots);
    }

    @Bean
    AwaitProbeHandlers.ActorAwaitHandler actorAwaitHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.ActorAwaitHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.ActorJoinHandler actorJoinHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.ActorJoinHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.ActorJoinAwaitHandler actorJoinAwaitHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.ActorJoinAwaitHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.ActorPushNotifyAwaitHandler actorPushAwaitHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.ActorPushNotifyAwaitHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.ActorFastHandler actorFastHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.ActorFastHandler(evidence);
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: automatic-turn-dispatch-session --config <path>");
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
