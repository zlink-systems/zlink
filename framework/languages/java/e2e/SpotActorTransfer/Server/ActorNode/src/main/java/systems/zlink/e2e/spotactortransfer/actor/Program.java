package systems.zlink.e2e.spotactortransfer.actor;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.spotactortransfer.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ActorNodeOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.spotactortransfer.actor")
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
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    EvidenceStore evidenceStore(ActorNodeOptions config) {
        return new EvidenceStore(config.nodeRid(),
            config.logDirectory() + "/" + config.nodeRid() + ".evidence.log");
    }

    @Bean
    GateStore gateStore() {
        return new GateStore();
    }

    @Bean
    DomainStateStore domainStateStore(ActorNodeOptions config) {
        return new DomainStateStore(config.logDirectory() + "/domain-state");
    }

    @Bean
    ActorNodeHttpServer actorNodeHttpServer(
        ObjectMapper json,
        EvidenceStore evidence,
        GateStore gates,
        systems.zlink.framework.spots.ZLinkSpotManager spots,
            systems.zlink.framework.actors.ZLinkActorManager actors,
            systems.zlink.framework.actors.ZLinkActorClient actorClient,
            systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle lifecycle,
            ActorNodeOptions config) {
        int requiredPeerCount = java.util.Arrays.stream(config.meshPeers().split(","))
            .map(peer -> peer.split("=", 2))
            .filter(fields -> fields.length == 2 && !config.nodeRid().equals(fields[0]))
            .mapToInt(ignored -> 1)
            .sum();
        return new ActorNodeHttpServer(
            config.httpEndpoint(),
            json,
            evidence,
            gates,
            spots,
            actors,
            actorClient,
            requiredPeerCount,
            lifecycle);
    }

    @Bean
    ZLinkFrameworkConfigurer framework(
        EvidenceStore evidence,
        ActorNodeOptions config,
        ZLinkRedisRelocationStore relocationStore) {
        return options -> {
            String nodeRid = evidence.nodeRid();
            options.addRelocationStore(relocationStore);
            options.addHandlersFromPackageOf(TransferComponents.class);
            options.configureLocations().setRouteCacheMaxAge(Duration.ZERO);
            options.configureLocations().setMessageFollowDuration(Duration.ofSeconds(2));
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .setMessageFlowObserver(flow -> {
                    evidence.addFlow(flow);
                    return java.util.concurrent.CompletableFuture.completedFuture(null);
                })
                .traceLogFile(config.logDirectory() + "/" + nodeRid + "-flow.log")
                .traceLabel("java-spot-transfer-" + nodeRid);
            ZLinkMeshNodeBuilder node = options.addRouteMesh(Contracts.MESH);
            node.listen(config.meshEndpoint())
                .setRoutingId(RoutingId.from(nodeRid));
            if ("ST-A1".equals(config.scenario())) {
                node.setPlacementWeight("actor-a".equals(nodeRid) ? 100 : 0);
            }
            if (!config.automaticTopology()) {
                for (String peer : config.meshPeers().split(",")) {
                    String[] fields = peer.split("=", 2);
                    if (fields.length == 2 && !nodeRid.equals(fields[0])) {
                        node.peerConnections().connect(
                            RoutingId.from(fields[0]), fields[1]);
                    }
                }
            }
            node.objects().client();
            ZLinkMeshObjectServerBuilder objects = node.objects().server();
            objects.addEntrySpot(TransferComponents.TransferEntrySpot.class);
            registerActor(objects, Contracts.STATEFUL, true);
            registerActor(objects, Contracts.EMPTY_STATE, true);
            registerActor(objects, Contracts.NO_ADAPTER, false);
            registerActor(objects, Contracts.FAIL_OUT, true);
            registerActor(objects, Contracts.FAIL_LEAVE, true);
            registerActor(objects, Contracts.FAIL_IN, true);
            objects.addSpotFactory(
                TransferComponents.TransferUserSpot.class.getName(),
                TransferComponents.TransferUserSpot.class,
                factory -> factory.disableRelocation());
            options.addStreamNode("spot-transfer-session-" + nodeRid)
                .bind(config.streamEndpoint())
                .enableActorDispatch()
                .registerSession(TransferComponents.TransferSession.class);
        };
    }

    private static void registerActor(
        ZLinkMeshObjectServerBuilder objects,
        String actorType,
        boolean adapter) {
        objects.addActorFactory(
            actorType,
            TransferComponents.TransferActor.class,
            TransferComponents.TransferActorFactory.class,
            factory -> {
                if (adapter) {
                    factory.preserveStateWith(
                        TransferComponents.TransferActorAdapter.class);
                } else {
                    factory.recreateOnRelocation();
                }
            });
    }

    @Bean
    ZLinkRedisLocationStore locationStore(ActorNodeOptions config) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(config.redisLocationEndpoint())
            .setKeyPrefix(config.locationKeyPrefix()));
    }

    @Bean
    ZLinkRedisRelocationStore relocationStore(ActorNodeOptions config) {
        return new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions()
            .setConnectionString(config.redisLocationEndpoint())
            .setKeyPrefix(config.locationKeyPrefix() + "relocation:"));
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: spot-actor-transfer-node --config <path>");
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
