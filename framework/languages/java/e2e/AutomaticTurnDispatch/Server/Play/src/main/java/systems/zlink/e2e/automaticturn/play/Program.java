package systems.zlink.e2e.automaticturn.play;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Path;
import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.automaticturn.shared.Contracts;
import systems.zlink.e2e.automaticturn.shared.EnsureSpotHandler;
import systems.zlink.e2e.automaticturn.shared.EvidenceHttpServer;
import systems.zlink.e2e.automaticturn.shared.EvidenceStore;
import systems.zlink.e2e.automaticturn.shared.PlayBindActorsHandler;
import systems.zlink.e2e.automaticturn.shared.AwaitActorFactory;
import systems.zlink.e2e.automaticturn.shared.AwaitActor;
import systems.zlink.e2e.automaticturn.shared.AwaitEntrySpot;
import systems.zlink.e2e.automaticturn.shared.AwaitProbeHandlers;
import systems.zlink.e2e.automaticturn.shared.AwaitProbeSpot;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import io.micrometer.core.instrument.MeterRegistry;
import systems.zlink.e2e.automaticturn.shared.DrainEvidence;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

@EnableZLinkFramework
@EnableConfigurationProperties(PlayOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.automaticturn.play")
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
    EvidenceStore evidenceStore(PlayOptions config) {
        return new EvidenceStore(config.nodeRid());
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
        ZLinkSpotManager spots,
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        PlayOptions config) {
        drainEvidence.observe(lifecycle.observe());
        return new EvidenceHttpServer(
            evidence, json, config.httpEndpoint(), metrics,
            lifecycle, lifecycle::monitoringLocationRuntimeQuery, drainEvidence, spots::close,
            () -> routes.requestToNode(
                    Contracts.ROUTE_CHANNEL,
                    RoutingId.from(Contracts.PLAY_NODE_B),
                    new Contracts.EnsureSpotReq("obs-c5-source-route-ready"))
                .timeout(java.time.Duration.ofSeconds(30))
                .submit(Contracts.EnsureSpotRes.class)
                .thenApply(Contracts.EnsureSpotRes::nodeRid));
    }

    @Bean
    DrainEvidence drainEvidence() { return new DrainEvidence(); }

    @Bean(destroyMethod = "close")
    systems.zlink.e2e.automaticturn.shared.PersistentRoomEvents persistentRoomEvents(PlayOptions config) {
        return new systems.zlink.e2e.automaticturn.shared.PersistentRoomEvents(
            config.redisLocationEndpoint(), config.locationKeyPrefix());
    }

    @Bean
    ZLinkFrameworkConfigurer framework(PlayOptions config) {
        return options -> {
            String nodeRid = config.nodeRid();
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(config.logDirectory() + "/" + nodeRid + "-flow.log")
                .traceLabel("java-atd-" + nodeRid);
            ZLinkMeshNodeBuilder mesh = options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(config.routeEndpoint())
                .setRoutingId(RoutingId.from(nodeRid));
            mesh.channelName(Contracts.ROUTE_CHANNEL);
            String routePeerEndpoint = config.routePeerEndpoint();
            if (!routePeerEndpoint.isBlank()) {
                mesh.peerConnections().connect(routePeerEndpoint);
            }
            mesh.addRouteRequestHandler(
                PlayBindActorsHandler.class,
                Contracts.BindActorsReq.class,
                Contracts.BindActorsRes.class);
            mesh.addRouteRequestHandler(
                EnsureSpotHandler.Play.class,
                Contracts.EnsureSpotReq.class,
                Contracts.EnsureSpotRes.class);
            options.addClientServerChannel(Contracts.DELAY_CHANNEL)
                .enableClient(config.delayEndpoint());
            String fanoutEndpoint = config.observabilityFanoutEndpoint();
            if (!fanoutEndpoint.isBlank()) {
                var fanout = options.addFanoutChannel(Contracts.OBS_FANOUT_CHANNEL);
                if (Contracts.PLAY_NODE_A.equals(nodeRid)) {
                    fanout.enablePublisher(fanoutEndpoint);
                }
                fanout.connect(fanoutEndpoint)
                    .addPublishHandler(
                        AwaitProbeHandlers.ObservabilityFanoutHandler.class,
                        Contracts.ObservabilityFanoutEvent.class);
            }
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
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(PlayOptions config) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(config.redisLocationEndpoint())
            .setKeyPrefix(config.locationKeyPrefix()));
    }

    @Bean
    ApplicationRunner createProbeSpot(ZLinkSpotManager spots, PlayOptions config) {
        return ignored -> {
            if (!Contracts.PLAY_NODE_A.equals(config.nodeRid())) {
                return;
            }
            spots.getOrCreate(
                    AwaitProbeSpot.class,
                    RoutingId.from(Contracts.TARGET_SPOT),
                    ZLinkMessage.of("bootstrap"))
                .whenComplete((created, failure) -> {
                    if (failure != null) {
                        System.getLogger(Program.class.getName()).log(
                            System.Logger.Level.ERROR,
                            "probe spot bootstrap failed",
                            failure);
                    }
                });
        };
    }

    @Bean
    AwaitProbeHandlers.HoldHandler holdHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.HoldHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.ObservabilityQueueHandler observabilityQueueHandler() {
        return new AwaitProbeHandlers.ObservabilityQueueHandler();
    }

    @Bean
    AwaitProbeHandlers.AwaitHandler awaitHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.AwaitHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.WorkerAwaitHandler workerAwaitHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.WorkerAwaitHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.ProbeHandler probeHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.ProbeHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.WorkerAwaitMsgHandler workerAwaitMsgHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.WorkerAwaitMsgHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.ProbeMsgHandler probeCommandHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.ProbeMsgHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.AwaitMsgHandler awaitCommandHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.AwaitMsgHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.AwaitTimeoutMsgHandler awaitTimeoutCommandHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.AwaitTimeoutMsgHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.AwaitCancelMsgHandler awaitCancelCommandHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.AwaitCancelMsgHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.RemoteSpotAwaitHandler remoteSpotAwaitHandler(
        EvidenceStore evidence,
        SpotHandleResolver spots) {
        return new AwaitProbeHandlers.RemoteSpotAwaitHandler(evidence, spots);
    }

    @Bean
    AwaitProbeHandlers.TimerStartMsgHandler timerStartCommandHandler() {
        return new AwaitProbeHandlers.TimerStartMsgHandler();
    }

    @Bean
    AwaitProbeHandlers.TimerStopMsgHandler timerStopCommandHandler() {
        return new AwaitProbeHandlers.TimerStopMsgHandler();
    }

    @Bean
    AwaitProbeHandlers.TimerTickHandler timerTickHandler(
        EvidenceStore evidence,
        ZLinkFanoutClient fanout,
        PlayOptions config) {
        return new AwaitProbeHandlers.TimerTickHandler(
            evidence, fanout, !config.observabilityFanoutEndpoint().isBlank());
    }

    @Bean
    AwaitProbeHandlers.ObservabilityFanoutHandler observabilityFanoutHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.ObservabilityFanoutHandler(evidence);
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
    AwaitProbeHandlers.SpotActorJoinHandler spotActorJoinHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.SpotActorJoinHandler(evidence);
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

    @Bean
    PlayBindActorsHandler playBindActorsHandler(
        systems.zlink.framework.actors.ZLinkActorManager actors,
        ZLinkSpotManager spots,
        EvidenceStore evidence) {
        return new PlayBindActorsHandler(actors, spots, evidence);
    }

    @Bean
    EnsureSpotHandler.Play playEnsureSpotHandler(
        ZLinkSpotManager spots,
        EvidenceStore evidence) {
        return new EnsureSpotHandler.Play(spots, evidence);
    }

    @Bean
    AwaitProbeHandlers.SpotActorAwaitHandler spotActorAwaitHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.SpotActorAwaitHandler(evidence);
    }

    @Bean
    AwaitProbeHandlers.SpotActorFastHandler spotActorFastHandler(EvidenceStore evidence) {
        return new AwaitProbeHandlers.SpotActorFastHandler(evidence);
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: automatic-turn-dispatch-play --config <path>");
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
