package systems.zlink.e2e.spotservice.publisher;

import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;

@EnableZLinkFramework
@EnableConfigurationProperties(PublisherOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.spotservice.publisher")
public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        String config = configPath(args);
        ConfigurableApplicationContext context =
            new SpringApplicationBuilder(Program.class)
                .environment(isolatedEnvironment())
                .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
                .web(WebApplicationType.NONE)
                .run();
        try {
            ZLinkSpotPublisherClient publisher = context.getBean(ZLinkSpotPublisherClient.class);
            publisher.publish(
                    Contracts.ROUTE_CHANNEL,
                    "spot.events",
                    new Contracts.MeshMsg("c4-publisher"))
                .submit();
            System.out.println("scenario SM-C4 passed");
        } finally {
            context.close();
        }
    }

    @Bean
    systems.zlink.framework.spring.ZLinkFrameworkConfigurer publisherFramework(PublisherOptions publisher) {
        return options -> {
            String logDir = publisher.logDir();
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/publisher-flow.log")
                .traceLabel("java-sm-publisher");
            var mesh = options.addRouteMesh(Contracts.SPOT_MESH)
                .listen(publisher.spotPublisherEndpoint())
                .setRoutingId(RoutingId.from("publisher"));
            mesh.channelName(Contracts.ROUTE_CHANNEL).client();
            mesh.configureSpotPublisher();
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(PublisherOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: spot-service-publisher --config <path>");
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
