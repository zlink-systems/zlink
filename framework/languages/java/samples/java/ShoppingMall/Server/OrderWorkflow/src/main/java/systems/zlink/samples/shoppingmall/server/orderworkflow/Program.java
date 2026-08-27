package systems.zlink.samples.shoppingmall.server.orderworkflow;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.time.Duration;
import systems.zlink.contracts.core.RoutingId;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.samples.shoppingmall.server.configuration.SampleLocationStore;
import systems.zlink.samples.shoppingmall.server.configuration.SampleNames;
import systems.zlink.samples.shoppingmall.server.configuration.SampleTopology;
import systems.zlink.samples.shoppingmall.server.orderworkflow.spots.OrderWorkflowSpot;
import systems.zlink.samples.shoppingmall.server.shared.store.RedisCommerceStore;

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology.class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = Program.class)
public final class Program {
    private Program() {
    }

    public static void main(String[] args) throws Exception {
        ConfigurableApplicationContext app = run(SampleTopology.configPath(args));
        HttpServer http = startHttp(
            app.getBean(SampleTopology.class),
            app.getBean(ZLinkFrameworkRuntime.class));
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            http.stop(0);
            try {
                app.close();
            } catch (Exception ignored) {
            }
        }));
    }

    public static ConfigurableApplicationContext run(String configPath) {
        StandardEnvironment environment = new StandardEnvironment();
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME);
        environment.getPropertySources().remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(Program.class)
            .environment(environment)
            .properties("spring.config.location=" + Path.of(configPath).toAbsolutePath().toUri())
            .web(WebApplicationType.NONE);
        builder.application().setKeepAlive(true);
        return builder.run();
    }

    @Bean
    ZLinkFrameworkConfigurer orderWorkflowFramework(SampleTopology topology) {
        SampleTopology.Workflow workflow = topology.workflow();
        return options -> {
            options.configureLocations();
            options.addLocationStore(SampleLocationStore.create(topology));
            options.addRelocationStore(new ZLinkRedisRelocationStore(
                new ZLinkRedisRelocationOptions()
                    .setConnectionString(topology.location().redisEndpoint())
                    .setKeyPrefix(topology.location().redisKeyPrefix() + "relocation:")));
            options.addHandlersFromPackageOf(Program.class);
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            ZLinkMeshNodeBuilder node = options.addRouteMesh(SampleNames.OrderSpotDiscovery);
            node.listen(workflow.spotRouterEndpoint())
                .setRoutingId(RoutingId.from(workflow.instanceName()));
            node.objects()
                .server()
                .addInstanceSpotFactory(
                    SampleNames.OrderWorkflowSpotType,
                    OrderWorkflowSpot.class,
                    factory -> factory.recreateOnRelocation());
        };
    }

    @Bean(destroyMethod = "close")
    RedisCommerceStore redisCommerceStore(SampleTopology topology) {
        RedisCommerceStore store = new RedisCommerceStore(topology);
        store.seedDefaults();
        return store;
    }

    @Bean
    OrderWorkflowService orderWorkflowService(RedisCommerceStore store) {
        return new OrderWorkflowService(store);
    }

    private static HttpServer startHttp(SampleTopology topology, ZLinkFrameworkRuntime runtime) throws IOException {
        ObjectMapper json = new ObjectMapper();
        URI uri = URI.create(topology.workflow().httpUrl());
        HttpServer server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
        server.createContext("/health", exchange -> {
            byte[] bytes = json.writeValueAsString(new Health("ok")).getBytes(StandardCharsets.UTF_8);
            exchange.getResponseHeaders().add("content-type", "application/json");
            exchange.sendResponseHeaders(200, bytes.length);
            exchange.getResponseBody().write(bytes);
            exchange.close();
        });
        server.createContext("/self-check/relocate", exchange -> {
            if (!"POST".equals(exchange.getRequestMethod())) {
                writeJson(exchange, json, 405, new ErrorBody("method not allowed"));
                return;
            }
            runtime.relocate(new ZLinkFrameworkRelocationOptions(
                    ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                    null,
                    Duration.ofSeconds(30)))
                .whenComplete((result, failure) -> {
                    try {
                        if (failure == null) {
                            writeJson(exchange, json, 200, new RelocationResult(result.outcome().name()));
                        } else {
                            writeJson(exchange, json, 500, new ErrorBody(failure.getMessage()));
                        }
                    } catch (IOException ignored) {
                        exchange.close();
                    }
                });
        });
        server.start();
        return server;
    }

    private record Health(String status) {
    }

    private record RelocationResult(String outcome) {
    }

    private record ErrorBody(String error) {
    }

    private static void writeJson(HttpExchange exchange, ObjectMapper json, int status, Object body) throws IOException {
        byte[] bytes = json.writeValueAsString(body).getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("content-type", "application/json");
        exchange.sendResponseHeaders(status, bytes.length);
        exchange.getResponseBody().write(bytes);
        exchange.close();
    }
}
