package systems.zlink.e2e.kotlin.automaticturn;

import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
public final class SessionApplication {
    private SessionApplication() {
    }

    public static AutoCloseable run(String... args) {
        SpringApplicationBuilder builder = new SpringApplicationBuilder(SessionApplication.class)
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run(args)::close;
    }

    @Bean
    ZLinkFrameworkConfigurer framework(ZLinkRedisRelocationStore relocationStore) {
        return options -> {
            options.addRelocationStore(relocationStore);
            String nodeRid = Env.get("nodeRid", "session-a");
            String logDir = Env.get("logDirectory", "logs");
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/session-flow.log")
                .traceLabel("kotlin-atd-session");
            ZLinkMeshNodeBuilder mesh = options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(Env.get("sessionRouteEndpoint"))
                .setRoutingId(RoutingId.from(nodeRid))
                // Session relays requests and is not a User Spot placement target.
                .setPlacementWeight(0);
            mesh.channelName(Contracts.ROUTE_CHANNEL).server();
            mesh.peerConnections().connect(Env.get("playRouteEndpoint"));
            String playBRoute = Env.get("playBRouteEndpoint", "");
            if (!playBRoute.isBlank()) {
                mesh.peerConnections().connect(playBRoute);
            }
            options.addClientServerChannel(Contracts.DELAY_CHANNEL)
                .client()
                .connect(Env.get("delayEndpoint"));
            mesh.objects()
                .server()
                .addEntrySpot(ProbeEntrySpot.class)
                .addActorFactory(
                    "probe",
                    ProbeActor.class,
                    ProbeActorFactory.class,
                    factory -> factory.recreateOnRelocation());
            options.addStreamNode("gateway")
                .bind(Env.get("streamEndpoint"))
                .enableActorDispatch()
                .registerSession(ProbeSession.class)
                .addSessionPacketHandler(ActorAuthReqHandler.class)
                .addSessionPacketHandler(BindActorsReqHandler.class)
                .addSessionPacketHandler(EnsureSpotReqHandler.class)
                .addSessionPacketHandler(RemoteSpotAwaitReqRouteHandler.class)
                .addSessionPacketHandler(ShutdownAwaitReqRouteHandler.class)
                .addSessionPacketHandler(ShutdownRecoveryReqRouteHandler.class)
                .addSessionPacketHandler(ProbeReqRouteHandler.class)
                .addSessionPacketHandler(CleanupProbeReqRouteHandler.class)
                .addSessionPacketHandler(HoldMsgRouteHandler.class)
                .addSessionPacketHandler(AwaitMsgRouteHandler.class)
                .addSessionPacketHandler(WorkerAwaitMsgRouteHandler.class)
                .addSessionPacketHandler(ProbeMsgRouteHandler.class)
                .addSessionPacketHandler(TimerStartMsgRouteHandler.class)
                .addSessionPacketHandler(TimerStopMsgRouteHandler.class)
                .addSessionPacketHandler(AwaitTimeoutMsgRouteHandler.class)
                .addSessionPacketHandler(AwaitTimeoutReqRouteHandler.class)
                .addSessionPacketHandler(AwaitCancelMsgRouteHandler.class)
                .addSessionPacketHandler(SpotProbeMsgRouteHandler.class)
                .addSessionPacketHandler(EvidenceReqRouteHandler.class);
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore() {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(Env.get("redisLocationEndpoint"))
            .setKeyPrefix(Env.get("locationKeyPrefix")));
    }

    @Bean
    ZLinkRedisRelocationStore relocationStore() {
        return new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions()
            .setConnectionString(Env.get("redisLocationEndpoint"))
            .setKeyPrefix(Env.get("locationKeyPrefix") + "relocation:"));
    }
}
