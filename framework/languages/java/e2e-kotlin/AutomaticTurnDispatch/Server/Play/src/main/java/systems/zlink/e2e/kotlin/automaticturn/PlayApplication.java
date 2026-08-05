package systems.zlink.e2e.kotlin.automaticturn;

import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.spots.ZLinkSpotManager;

@EnableZLinkFramework
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.kotlin.automaticturn.play")
public final class PlayApplication {
    private PlayApplication() {
    }

    public static AutoCloseable run(String... args) {
        SpringApplicationBuilder builder = new SpringApplicationBuilder(PlayApplication.class)
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run(args)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer framework() {
        return options -> {
            String nodeRid = Env.get("nodeRid", "play-a");
            String logDir = Env.get("logDirectory", "logs");
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/" + nodeRid + "-flow.log")
                .traceLabel("kotlin-atd-" + nodeRid);
            ZLinkMeshNodeBuilder mesh = options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(Env.get("playRouteEndpoint"))
                .setRoutingId(RoutingId.from(nodeRid));
            mesh.channelName(Contracts.SPOT_MESH);
            mesh.peerConnections().connect(Env.get("sessionRouteEndpoint"));
            mesh.addRouteRequestHandler(
                EvidenceRouteRequestHandler.class,
                Contracts.EvidenceReq.class,
                Contracts.EvidenceRes.class);
            mesh.addRouteRequestHandler(
                EnsureSpotRouteRequestHandler.class,
                Contracts.EnsureSpotReq.class,
                Contracts.EnsureSpotRes.class);
            mesh.addRouteRequestHandler(
                PlayBindActorsHandler.class,
                Contracts.BindActorsReq.class,
                Contracts.BindActorsRes.class);
            options.addClientServerChannel(Contracts.DELAY_CHANNEL)
                .enableClient(Env.get("delayEndpoint"));
            mesh.objects()
                .server()
                .addEntrySpot(ProbeEntrySpot.class)
                .addSpotFactory(
                    "probe",
                    ProbeSpot.class,
                    factory -> factory.disableRelocation())
                .addActorFactory(
                    "probe",
                    ProbeActor.class,
                    ProbeActorFactory.class,
                    factory -> factory.recreateOnRelocation());
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore() {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(Env.get("redisLocationEndpoint"))
            .setKeyPrefix(Env.get("locationKeyPrefix")));
    }

    @Bean
    ApplicationRunner createSpots(ZLinkSpotManager spots) {
        return ignored -> {
            if (!"play-a".equals(Env.get("nodeRid", "play-a"))) {
                return;
            }
            spots.getOrCreate(ProbeSpot.class, RoutingId.from("room-a"), ZLinkMessage.of("bootstrap"))
                .toCompletableFuture()
                .join();
            spots.getOrCreate(ProbeSpot.class, RoutingId.from("room-b"), ZLinkMessage.of("bootstrap"))
                .toCompletableFuture()
                .join();
        };
    }

    @Bean
    PlayEvidenceStore evidence() {
        return new PlayEvidenceStore();
    }
}
