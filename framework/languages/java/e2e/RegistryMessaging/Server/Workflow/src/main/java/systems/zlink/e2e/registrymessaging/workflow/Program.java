package systems.zlink.e2e.registrymessaging.workflow;

import java.util.concurrent.CompletableFuture;
import java.nio.file.Path;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.registrymessaging.workflow.Configuration.ServerOptions;
import systems.zlink.e2e.registrymessaging.workflow.Handlers.ProfileMsgHandler;
import systems.zlink.e2e.registrymessaging.workflow.Handlers.ProfileReqHandler;
import systems.zlink.e2e.registrymessaging.workflow.Handlers.RouteReqHandler;
import systems.zlink.e2e.registrymessaging.workflow.Handlers.WorkflowReqHandler;
import systems.zlink.e2e.registrymessaging.workflow.Infrastructure.ScenarioState;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@EnableConfigurationProperties(ServerOptions.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = "systems.zlink.e2e.registrymessaging.workflow")
public final class Program {
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
    ScenarioState scenarioState(ServerOptions options) {
        return new ScenarioState(options.providerRid(), options.providerInstance());
    }

    @Bean
    ZLinkFrameworkConfigurer workflowFramework(ScenarioState state, ServerOptions server) {
        return options -> {
            String logDir = server.logDir();
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(logDir + "/" + state.providerRid() + "-flow.log")
                .traceLabel("java-rm-" + state.providerRid())
                .setMessageFlowObserver(error -> {
                    if (error.outcome() != ZLinkMessageFlowOutcome.ERROR) {
                        return CompletableFuture.completedFuture(null);
                    }
                    state.record(
                        "dispatch-error",
                        error.errorReason() + "/" + error.errorAction() + "/" + error.packetName());
                    return CompletableFuture.completedFuture(null);
                });
            options.addHandlersFromPackageOf(ProfileReqHandler.class);

            String apiEndpoint = server.apiEndpoint();
            if (!apiEndpoint.isBlank()) {
                var api = options.addClientServerChannel(Contracts.API_CHANNEL);
                var endpoint = java.net.URI.create(apiEndpoint);
                api.server()
                    .setBindHost(endpoint.getHost())
                    .setAdvertiseHost(endpoint.getHost())
                    .listen(endpoint.getPort())
                    .addHandlerGroup(Contracts.HANDLER_GROUP);
            }

            String workflowEndpoint = server.workflowEndpoint();
            if (!workflowEndpoint.isBlank()) {
                var workflow = options.addClientServerChannel(Contracts.WORKFLOW_CHANNEL);
                var endpoint = java.net.URI.create(workflowEndpoint);
                workflow.server()
                    .setBindHost(endpoint.getHost())
                    .setAdvertiseHost(endpoint.getHost())
                    .listen(endpoint.getPort())
                    .addHandlerGroup(Contracts.HANDLER_GROUP);
            }

            String routeEndpoint = server.routeEndpoint();
            if (!routeEndpoint.isBlank()) {
                var route = options.addRouteMesh(Contracts.ROUTE_CHANNEL)
                    .listen(routeEndpoint)
                    .setRoutingId(RoutingId.from(state.providerRid()));
                route.channelName(Contracts.ROUTE_CHANNEL).server();
                route.addRouteRequestHandler(
                    RouteReqHandler.class,
                    Contracts.RouteReq.class,
                    Contracts.RouteRes.class);
            }
        };
    }

    @Bean
    ZLinkRedisLocationStore locationStore(ServerOptions options) {
        return new ZLinkRedisLocationStore(new ZLinkRedisLocationOptions()
            .setConnectionString(options.redisLocationEndpoint())
            .setKeyPrefix(options.locationKeyPrefix()));
    }

    @Bean
    ApplicationRunner applyInitialSocketWeight(ZLinkChannelRuntimeOptions runtimeOptions, ServerOptions options) {
        return ignored -> {
            String weight = options.apiWeight();
            if (!weight.isBlank()) {
                runtimeOptions
                    .clientServerChannel(Contracts.API_CHANNEL)
                    .configureServerSocket()
                    .weight(Integer.parseInt(weight));
            }
        };
    }

    @Bean
    ProfileReqHandler profileRequestHandler(ScenarioState state) {
        return new ProfileReqHandler(state);
    }

    @Bean
    ProfileMsgHandler profileCommandHandler(ScenarioState state) {
        return new ProfileMsgHandler(state);
    }

    @Bean
    RouteReqHandler routePingHandler(ScenarioState state) {
        return new RouteReqHandler(state);
    }

    @Bean
    WorkflowReqHandler workflowRequestHandler(ScenarioState state) {
        return new WorkflowReqHandler(state);
    }

    private static String configPath(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: registry-messaging-workflow --config <path>");
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
