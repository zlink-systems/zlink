package systems.zlink.e2e.pubsub.subscriber;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import java.nio.file.Path;
import systems.zlink.e2e.pubsub.shared.Contracts;
import systems.zlink.e2e.pubsub.subscriber.Configuration.SubscriberOptions;
import systems.zlink.e2e.pubsub.subscriber.Endpoints.OperationalEndpoints;
import systems.zlink.e2e.pubsub.subscriber.Handlers.EventMsgHandler;
import systems.zlink.e2e.pubsub.subscriber.Handlers.EvidenceDispatchErrorObserver;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.EvidenceStore;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.FanoutObserverController;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.SubscriberConnections;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.monitoring.ZLinkFanoutRuntime;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

@EnableZLinkFramework
@EnableConfigurationProperties(SubscriberOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.pubsub.subscriber")
public final class SubscriberApplication {
    public AutoCloseable run(String... args) {
        String configPath = configPath(args);
        StandardEnvironment environment = isolatedEnvironment();
        SpringApplicationBuilder builder = new SpringApplicationBuilder(SubscriberApplication.class)
            .environment(environment)
            .properties("spring.config.location=" + Path.of(configPath).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run()::close;
    }

    @Bean
    EvidenceStore evidenceStore(SubscriberOptions options) {
        return new EvidenceStore(options);
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    SubscriberConnections subscriberConnections() {
        return new SubscriberConnections();
    }

    @Bean
    FanoutObserverController fanoutObserverController(
        ObjectProvider<ZLinkFrameworkRuntime> runtime) {
        return new FanoutObserverController(runtime);
    }

    @Bean
    OperationalEndpoints operationalEndpoints(
        SubscriberOptions options,
        EvidenceStore evidence,
        ObjectMapper json,
        ZLinkFanoutRuntime fanoutRuntime,
        SubscriberConnections connections,
        FanoutObserverController observers) {
        return new OperationalEndpoints(
            options, evidence, json, fanoutRuntime, connections, observers);
    }

    @Bean
    EvidenceDispatchErrorObserver evidenceDispatchErrorObserver(EvidenceStore evidence) {
        return new EvidenceDispatchErrorObserver(evidence);
    }

    @Bean
    ZLinkFrameworkConfigurer subscriberFramework(
        SubscriberOptions options,
        EvidenceStore evidence,
        EvidenceDispatchErrorObserver observer,
        SubscriberConnections connections) {
        return framework -> {
            framework.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(options.logDir() + "/" + evidence.subscriberRid() + "-flow.log")
                .traceLabel("java-ps-" + evidence.subscriberRid())
                .setMessageFlowObserver(observer::observe);
            framework.addHandlersFromPackageOf(EventMsgHandler.class);
            var channel = framework.addFanoutChannel(Contracts.EVENT_CHANNEL);
            if (options.mixedMode()) {
                channel.enableSubscriber().subscriberConnections()
                    .connect(options.manualEndpoint());
            } else if (options.manualEndpoint() != null
                && !options.manualEndpoint().isBlank()) {
                channel.connect(options.manualEndpoint());
            } else {
                channel.enableSubscriber();
            }
            connections.install(channel.subscriberConnections());
            channel.addHandlerGroup(Contracts.HANDLER_GROUP);
        };
    }

    @Bean
    EventMsgHandler eventMsgHandler(EvidenceStore evidence) {
        return new EventMsgHandler(evidence);
    }

    @Bean
    @ConditionalOnProperty(prefix = "e2e", name = "redis-location-endpoint")
    ZLinkRedisLocationStore locationStore(SubscriberOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: pub-sub-subscriber --config <path>");
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
