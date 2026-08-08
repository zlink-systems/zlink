package systems.zlink.e2e.pubsub.publisher;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.core.env.StandardEnvironment;
import java.nio.file.Path;
import systems.zlink.e2e.pubsub.publisher.Configuration.PublisherOptions;
import systems.zlink.e2e.pubsub.publisher.Endpoints.PublisherEndpoints;
import systems.zlink.e2e.pubsub.publisher.Infrastructure.EvidenceStore;
import systems.zlink.e2e.pubsub.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

@EnableZLinkFramework
@EnableConfigurationProperties(PublisherOptions.class)
@SpringBootApplication(proxyBeanMethods = false)
public final class PublisherApplication {
    public AutoCloseable run(String... args) {
        String configPath = configPath(args);
        StandardEnvironment environment = isolatedEnvironment();
        SpringApplicationBuilder builder = new SpringApplicationBuilder(PublisherApplication.class)
            .environment(environment)
            .properties("spring.config.location=" + Path.of(configPath).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run()::close;
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    EvidenceStore evidenceStore() {
        return new EvidenceStore();
    }

    @Bean
    PublisherEndpoints publisherEndpoints(
        PublisherOptions options,
        systems.zlink.framework.channels.ZLinkFanoutClient fanout,
        EvidenceStore evidence,
        ObjectMapper json,
        systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle drain,
        ConfigurableApplicationContext application,
        ObjectProvider<ZLinkFrameworkRuntime> runtime) {
        return new PublisherEndpoints(
            options, fanout, evidence, json, drain, application, runtime);
    }

    @Bean
    ZLinkFrameworkConfigurer publisherFramework(PublisherOptions options) {
        return framework -> {
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            var channel = framework.addFanoutChannel(options.channelName());
            if (options.routingId() != null && !options.routingId().isBlank()) {
                channel.setRoutingId(RoutingId.from(options.routingId()));
            }
            if (options.routingIdPrefix() != null && !options.routingIdPrefix().isBlank()) {
                channel.setRoutingIdPrefix(options.routingIdPrefix());
            }
            if (options.advertiseHost() != null && !options.advertiseHost().isBlank()) {
                channel.setAdvertiseHost(options.advertiseHost());
            }
            if (options.publisherPort() != null) {
                channel.enablePublisher(options.publisherPort());
            } else {
                channel.enablePublisher(options.publisherEndpoint());
            }
        };
    }

    @Bean
    @ConditionalOnProperty(prefix = "e2e", name = "redis-location-endpoint")
    ZLinkRedisLocationStore locationStore(PublisherOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: pub-sub-publisher --config <path>");
        }
        return args[1];
    }

    private static StandardEnvironment isolatedEnvironment() {
        StandardEnvironment environment = new StandardEnvironment();
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        return environment;
    }
}
