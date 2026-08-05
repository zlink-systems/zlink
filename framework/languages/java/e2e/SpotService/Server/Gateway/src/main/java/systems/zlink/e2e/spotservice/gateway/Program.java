package systems.zlink.e2e.spotservice.gateway;

import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.e2e.spotservice.shared.GatewayHealthHttpServer;
import systems.zlink.e2e.spotservice.shared.GatewayOperationSpot;
import systems.zlink.e2e.spotservice.shared.ScenarioState;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spots.ZLinkSpotManager;

@EnableZLinkFramework
@EnableConfigurationProperties(GatewayOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.spotservice.gateway")
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
    ScenarioState scenarioState() {
        return new ScenarioState("gateway");
    }

    @Bean
    GatewayHealthHttpServer gatewayHealthHttpServer(
        ZLinkSpotManager spots,
        systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime meshRuntime,
        GatewayOptions options) {
        return new GatewayHealthHttpServer(options.gatewayHttpEndpoint(), spots, meshRuntime);
    }

    @Bean
    systems.zlink.framework.spring.ZLinkFrameworkConfigurer gatewayFramework(GatewayOptions gateway) {
        return options -> {
            String logDir = gateway.logDir();
            String gatewayRid = gateway.gatewayRid();
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/gateway-flow.log")
                .traceLabel("java-sm-gateway");
            boolean spotOnly = gateway.spotOnly();
            ZLinkMeshNodeBuilder node = options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(spotOnly ? gateway.spotEndpoint() : gateway.routeEndpoint())
                .setRoutingId(RoutingId.from(gatewayRid));
            node.objects()
                .server()
                .addSpotFactory(
                    "gateway-operation",
                    GatewayOperationSpot.class,
                    factory -> factory.disableRelocation());
            if (spotOnly) {
                System.out.println("[topology] role=gateway route_mesh=enabled route_channel=disabled");
            } else {
                node.channelName(Contracts.ROUTE_CHANNEL).client();
            }
            options.addClientServerChannel(Contracts.EGRESS_CHANNEL).client();
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(GatewayOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: spot-service-gateway --config <path>");
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
