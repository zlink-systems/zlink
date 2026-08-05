package systems.zlink.e2e.spotservice.multinode;

import java.nio.file.Path;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.e2e.spotservice.shared.MultiNodeSpot;
import systems.zlink.e2e.spotservice.shared.ScenarioActorFactory;
import systems.zlink.e2e.spotservice.shared.ScenarioActor;
import systems.zlink.e2e.spotservice.shared.ScenarioEntrySpot;
import systems.zlink.e2e.spotservice.shared.ScenarioState;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(MultiNodeOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.spotservice.multinode")
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
    ScenarioState scenarioState(MultiNodeOptions options) {
        return new ScenarioState(options.nodeRid());
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    MultiNodeHttpServer multiNodeHttpServer(
        ScenarioState state,
        ObjectMapper json,
        systems.zlink.framework.spots.ZLinkSpotManager spots,
        systems.zlink.framework.channels.ZLinkRouteClient routes,
        ZLinkActorManager actors,
        ZLinkActorClient actorClient,
        systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime meshRuntime,
        MultiNodeOptions options) {
        return new MultiNodeHttpServer(
            state,
            json,
            options.httpEndpoint(),
            spots,
            routes,
            actors,
            actorClient,
            meshRuntime);
    }

    @Bean
    ZLinkFrameworkConfigurer multiNodeFramework(
        ScenarioState state,
        MultiNodeOptions multi,
        ZLinkRedisRelocationStore relocationStore) {
        return options -> {
            String nodeRid = state.nodeRid();
            String logDir = multi.logDir();
            options.addRelocationStore(relocationStore);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/" + nodeRid + "-flow.log")
                .traceLabel("java-sm-" + nodeRid);
            ZLinkMeshNodeBuilder node = options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(multi.spotOnly() ? multi.spotEndpoint() : multi.routeEndpoint())
                .setRoutingId(RoutingId.from(nodeRid));
            node.objects()
                .server()
                .addEntrySpot(ScenarioEntrySpot.class)
                .addActorFactory(
                    "scenario",
                    ScenarioActor.class,
                    ScenarioActorFactory.class,
                    factory -> factory.recreateOnRelocation())
                .addSpotFactory(
                    "multi-node",
                    MultiNodeSpot.class,
                    factory -> factory.disableRelocation());
            if (!multi.spotOnly()) {
                node.channelName(Contracts.ROUTE_CHANNEL).server();
            } else {
                System.out.println("[topology] role=" + nodeRid + " route_mesh=disabled");
            }
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(MultiNodeOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    @Bean
    ZLinkRedisRelocationStore relocationStore(MultiNodeOptions options) {
        return new ZLinkRedisRelocationStore(new ZLinkRedisRelocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: spot-service-multi-node --config <path>");
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
