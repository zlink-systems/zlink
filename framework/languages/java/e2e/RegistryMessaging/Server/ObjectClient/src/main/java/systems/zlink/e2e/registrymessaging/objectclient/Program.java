package systems.zlink.e2e.registrymessaging.objectclient;

import java.nio.file.Path;
import java.time.Duration;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ObjectClientOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.registrymessaging.objectclient")
public final class Program {
    private static final String RM_A3_MESH = "registry.messaging.rm-a3";
    private static final String RM_A3_SERVER_CHANNEL =
        "registry.messaging.rm-a3.server";

    private Program() {
    }

    public static void main(String[] args) {
        String config = configPath(args);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(isolatedEnvironment())
            .web(WebApplicationType.SERVLET)
            .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri());
        builder.application().setKeepAlive(true);
        builder.run();
    }

    @Bean
    ZLinkFrameworkConfigurer objectClientFramework(ObjectClientOptions client) {
        return options -> {
            var mesh = options.addRouteMesh(RM_A3_MESH)
                .listen(client.routeEndpoint())
                .setRoutingId(RoutingId.from(client.clientRid()))
                .setDefaultRequestTimeout(Duration.ofSeconds(2));
            mesh.objects().client();
            if (!client.serverWeight().isBlank()) {
                mesh.channelName(RM_A3_SERVER_CHANNEL)
                    .server()
                    .setWeight(Integer.parseInt(client.serverWeight()));
            } else {
                mesh.channelName(RM_A3_SERVER_CHANNEL).client();
            }
            for (String connection : client.peerConnections().split(",")) {
                if (connection.isBlank()) {
                    continue;
                }
                String[] fields = connection.split("@", 2);
                if (fields.length != 2 || fields[0].isBlank() || fields[1].isBlank()) {
                    throw new IllegalArgumentException(
                        "e2e.peer-connections must contain rid@endpoint entries");
                }
                mesh.peerConnections().connect(
                    RoutingId.from(fields[0]),
                    fields[1]);
            }
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(ObjectClientOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    static String meshName() {
        return RM_A3_MESH;
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException(
                "Usage: registry-messaging-object-client --config <path>");
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
