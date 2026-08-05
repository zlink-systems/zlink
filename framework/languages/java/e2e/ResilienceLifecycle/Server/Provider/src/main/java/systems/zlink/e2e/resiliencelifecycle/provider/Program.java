package systems.zlink.e2e.resiliencelifecycle.provider;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.time.Duration;
import java.nio.file.Path;
import java.util.concurrent.CompletableFuture;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.resiliencelifecycle.provider.endpoints.EvidenceHttpServer;
import systems.zlink.e2e.resiliencelifecycle.provider.handlers.WorkMsgHandler;
import systems.zlink.e2e.resiliencelifecycle.provider.handlers.WorkReqHandler;
import systems.zlink.e2e.resiliencelifecycle.provider.infrastructure.ScenarioState;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ProviderOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.resiliencelifecycle.provider")
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
    ScenarioState scenarioState(ProviderOptions options) {
        return new ScenarioState(options.providerRid());
    }

    @Bean
    ObjectMapper objectMapper() {
        return new ObjectMapper();
    }

    @Bean
    EvidenceHttpServer evidenceHttpServer(
        ScenarioState state,
        ObjectMapper json,
        ZLinkChannelRuntimeOptions runtimeOptions,
        ConfigurableApplicationContext applicationContext,
        systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle drain,
        ProviderOptions options) {
        return new EvidenceHttpServer(
            state,
            json,
            options.httpEndpoint(),
            runtimeOptions,
            applicationContext,
            drain);
    }

    @Bean
    ZLinkFrameworkConfigurer providerFramework(ScenarioState state, ProviderOptions provider) {
        return options -> {
            String logDir = provider.logDir();
            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(500));
            options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(2));
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/" + state.providerRid() + "-flow.log")
                .traceLabel("java-rl-" + state.providerRid())
                .setMessageFlowObserver(error -> {
                    if (error.outcome() != ZLinkMessageFlowOutcome.ERROR) {
                        return CompletableFuture.completedFuture(null);
                    }
                    state.record(
                        "DispatchError",
                        error.errorReason() + "/" + error.errorAction() + "/" + error.packetName());
                    if (state.observerThrows()) {
                        throw new IllegalStateException("dispatch observer failure");
                    }
                    return CompletableFuture.completedFuture(null);
                });
            options.addHandlersFromPackageOf(WorkReqHandler.class);
            options.addClientServerChannel(Contracts.CHANNEL)
                .enableServer(provider.apiEndpoint())
                .setRoutingId(RoutingId.from(state.providerRid()))
                .addHandlerGroup(Contracts.HANDLER_GROUP);
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(ProviderOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    @Bean
    WorkReqHandler workRequestHandler(ScenarioState state) {
        return new WorkReqHandler(state);
    }

    @Bean
    WorkMsgHandler workCommandHandler(ScenarioState state) {
        return new WorkMsgHandler(state);
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: resilience-lifecycle-provider --config <path>");
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
