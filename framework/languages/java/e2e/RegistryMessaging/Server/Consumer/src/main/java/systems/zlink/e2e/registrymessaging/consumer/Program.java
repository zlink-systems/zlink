package systems.zlink.e2e.registrymessaging.consumer;

import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.e2e.registrymessaging.consumer.Configuration.ConsumerOptions;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ConsumerOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.registrymessaging.consumer")
public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        String config = configPath(args);
        new SpringApplicationBuilder(Program.class)
            .environment(isolatedEnvironment())
            .web(WebApplicationType.SERVLET)
            .properties("spring.config.location=" + Path.of(config).toAbsolutePath().toUri())
            .run();
    }

    @Bean
    ZLinkFrameworkConfigurer consumerFramework(ConsumerOptions consumer) {
        return options -> {
            String logDir = consumer.logDir();
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/" + consumer.consumerName() + "-flow.log")
                .traceLabel("java-rm-" + consumer.consumerName());

            String mode = consumer.consumerMode();
            var channel = options.addClientServerChannel(Contracts.API_CHANNEL);
            var client = channel.client();
            if ("discovery".equals(mode)) {
                // Discovery uses the location provider for endpoint selection.
                options.addClientServerChannel(Contracts.WORKFLOW_CHANNEL).client();
            } else {
                for (String endpoint : consumer.providerEndpoints().split(",")) {
                    if (!endpoint.isBlank()) {
                        client.connect(endpoint);
                    }
                }
            }
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
            throw new IllegalArgumentException("Usage: registry-messaging-consumer --config <path>");
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
