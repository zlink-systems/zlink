package systems.zlink.e2e.channelegress.role;

import java.net.URI;
import java.nio.file.Path;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.channelegress.shared.ChannelProbeRequestHandler;
import systems.zlink.e2e.channelegress.shared.Contracts;
import systems.zlink.e2e.channelegress.shared.EvidenceState;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(RoleOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.channelegress.role")
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
    EvidenceState evidenceState(RoleOptions options) {
        return new EvidenceState(options.role(), options.rid());
    }

    @Bean
    com.fasterxml.jackson.databind.ObjectMapper objectMapper() {
        return new com.fasterxml.jackson.databind.ObjectMapper();
    }

    @Bean
    ChannelEgressHttpServer httpServer(
        RoleOptions options,
        EvidenceState evidence,
        com.fasterxml.jackson.databind.ObjectMapper json,
        systems.zlink.framework.channels.ZLinkClient client,
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime routeRuntime,
        systems.zlink.framework.monitoring.ZLinkClientServerRuntime clientServerRuntime,
        systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle lifecycle) {
        return new ChannelEgressHttpServer(
            options,
            evidence,
            json,
            client,
            routes,
            routeRuntime,
            clientServerRuntime,
            lifecycle);
    }

    @Bean
    ZLinkFrameworkConfigurer roleFramework(RoleOptions role, EvidenceState evidence) {
        return options -> {
            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(500));
            options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(2));
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLabel("java-ch-" + role.rid())
                .traceLogFile(role.logDirectory() + "/" + role.rid() + "-flow.log")
                .setMessageFlowObserver(flow -> {
                    if (flow.outcome() == ZLinkMessageFlowOutcome.ERROR) {
                        evidence.add(
                            "dispatch-error",
                            flow.errorReason() + "/" + flow.errorAction() + "/" + flow.packetName());
                    }
                    return CompletableFuture.completedFuture(null);
                });
            options.addHandlersFromPackageOf(ChannelProbeRequestHandler.class);
            options.addHandlersFromPackageOf(SpotWorkflowHandler.class);

            String[] gameServers = role.gameServerNames();
            String[] gameClients = role.gameClientNames();
            if (gameServers.length > 0 || gameClients.length > 0
                || role.instanceSpot() || role.objectClient()) {
                ZLinkMeshNodeBuilder game = options.addRouteMesh(Contracts.GAME_MESH)
                    .listen(role.gameEndpoint())
                    .setRoutingId(RoutingId.from(role.rid()));
                connectPeers(
                    game,
                    role.gamePeerRidValues(),
                    role.gamePeerEndpointValues());
                registerRouteChannels(game, gameServers, gameClients);
                if (role.instanceSpot() || role.objectClient()) {
                    var objects = game.objects();
                    if (role.instanceSpot()) {
                        objects.server().addSpotFactory(
                            Contracts.INSTANCE_SPOT_TYPE,
                            Config12Spot.class,
                            factory -> factory.disableRelocation());
                    }
                    if (role.objectClient()) {
                        objects.client();
                    }
                }
            }

            String[] auditServers = role.auditServerNames();
            String[] auditClients = role.auditClientNames();
            if (auditServers.length > 0 || auditClients.length > 0) {
                ZLinkMeshNodeBuilder audit = options.addRouteMesh(Contracts.AUDIT_MESH)
                    .listen(role.auditEndpoint())
                    .setRoutingId(RoutingId.from(role.rid() + "-audit"));
                connectPeers(
                    audit,
                    role.auditPeerRidValues(),
                    role.auditPeerEndpointValues());
                registerRouteChannels(audit, auditServers, auditClients);
            }

            if (role.workflowClient() || role.workflowServer()) {
                var workflow = options.addClientServerChannel(Contracts.WORKFLOW_CHANNEL);
                if (role.workflowClient()) {
                    workflow.client();
                }
                if (role.workflowServer()) {
                    URI endpoint = URI.create(role.workflowEndpoint());
                    workflow.server()
                        .setBindHost(endpoint.getHost())
                        .setAdvertiseHost(endpoint.getHost())
                        .listen(role.workflowPort())
                        .setWeight(role.workflowWeight())
                        .addHandlerGroup(Contracts.HANDLER_GROUP);
                }
            }
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(RoleOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    private static void registerRouteChannels(
        ZLinkMeshNodeBuilder node,
        String[] servers,
        String[] clients) {
        for (String channel : servers) {
            node.channelName(channel)
                .server()
                .addHandlerGroup(Contracts.HANDLER_GROUP);
        }
        for (String channel : clients) {
            node.channelName(channel).client();
        }
    }

    private static void connectPeers(
        ZLinkMeshNodeBuilder node,
        String[] peerRids,
        String[] peerEndpoints) {
        if (peerRids.length != peerEndpoints.length) {
            throw new IllegalArgumentException("peer RID and endpoint counts must match");
        }
        for (int index = 0; index < peerRids.length; index++) {
            String rid = peerRids[index].trim();
            String endpoint = peerEndpoints[index].trim();
            if (!rid.isBlank() && !endpoint.isBlank()) {
                node.peerConnections().connect(RoutingId.from(rid), endpoint);
            }
        }
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: channel-egress-role --config <path>");
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
