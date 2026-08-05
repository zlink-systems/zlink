package systems.zlink.e2e.resiliencelifecycle.consumer;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.e2e.resiliencelifecycle.consumer.endpoints.ConsumerEndpoints;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ConsumerOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.resiliencelifecycle.consumer")
public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        String config = configPath(args);
        StandardEnvironment environment = isolatedEnvironment();
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(environment)
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
    ConsumerEndpoints consumerEndpoints(
        ObjectMapper json,
        systems.zlink.framework.channels.ZLinkClient client,
        ZLinkFrameworkLifecycle lifecycle,
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository locations,
        ConsumerOptions options) {
        return new ConsumerEndpoints(
            json,
            client,
            lifecycle,
            locations,
            options.httpEndpoint());
    }

    @Bean
    ZLinkFrameworkConfigurer consumerFramework(ConsumerOptions consumer) {
        return options -> {
            String logDir = consumer.logDir();
            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(500));
            options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(2));
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/consumer-flow.log")
                .traceLabel("java-rl-consumer");
            options.addClientServerChannel(Contracts.CHANNEL).client();
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(ConsumerOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: resilience-lifecycle-consumer --config <path>");
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
