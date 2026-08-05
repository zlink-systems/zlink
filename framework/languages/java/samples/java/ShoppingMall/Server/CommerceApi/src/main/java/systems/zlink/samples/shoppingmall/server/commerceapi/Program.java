package systems.zlink.samples.shoppingmall.server.commerceapi;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.ConfigurableApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.StandardEnvironment;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.samples.shoppingmall.server.configuration.SampleFlowLog;
import systems.zlink.samples.shoppingmall.server.configuration.SampleLocationStore;
import systems.zlink.samples.shoppingmall.server.configuration.SampleNames;
import systems.zlink.samples.shoppingmall.server.configuration.SampleTopology;
import systems.zlink.samples.shoppingmall.server.shared.store.RedisCommerceStore;
import systems.zlink.samples.shoppingmall.shared.contracts.Messages;

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
        HttpServer http = startHttp(app.getBean(CommerceApiService.class), app.getBean(SampleTopology.class));
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            http.stop(0);
            app.close();
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
    ZLinkFrameworkConfigurer commerceApiFramework(SampleTopology topology) {
        SampleTopology.Api api = topology.api();
        return options -> {
            options.configureLocations();
            options.addLocationStore(SampleLocationStore.create(topology));
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile(SampleFlowLog.path(api.logDirectory(), api.instanceName()))
                .traceLabel(api.instanceName());
            options.addRouteMesh(SampleNames.OrderSpotDiscovery)
                .setRoutingIdPrefix("shoppingmall-api")
                .listen()
                .objects().client();
        };
    }

    @Bean(destroyMethod = "close")
    RedisCommerceStore redisCommerceStore(SampleTopology topology) {
        RedisCommerceStore store = new RedisCommerceStore(topology);
        store.seedDefaults();
        return store;
    }

    @Bean
    CommerceApiService commerceApiService(
        RedisCommerceStore store,
        ZLinkRouteClient routes,
        SampleTopology topology) {
        return new CommerceApiService(store, routes, topology);
    }

    private static HttpServer startHttp(CommerceApiService api, SampleTopology topology) throws IOException {
        ObjectMapper json = new ObjectMapper().findAndRegisterModules();
        URI uri = URI.create(topology.api().httpUrl());
        HttpServer server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
        server.createContext("/health", exchange -> writeJson(exchange, json, 200, new Health("ok")));
        server.createContext("/", exchange -> route(exchange, json, api));
        server.start();
        return server;
    }

    private static void route(
        HttpExchange exchange,
        ObjectMapper json,
        CommerceApiService api) throws IOException {
        try {
            String method = exchange.getRequestMethod();
            String path = exchange.getRequestURI().getPath();
            if ("POST".equals(method) && "/orders/start".equals(path)) {
                Messages.StartOrderReq request = read(exchange, json, Messages.StartOrderReq.class);
                writeJson(exchange, json, api.startOrder(request));
                return;
            }
            if ("GET".equals(method) && path.startsWith("/orders/")) {
                writeJson(exchange, json, api.getOrder(last(path)));
                return;
            }
            if ("POST".equals(method) && path.startsWith("/self-check/projection/")
                && path.endsWith("/delete")) {
                api.deleteProjection(segment(path, 3));
                writeJson(exchange, json, 200, new Ok(true));
                return;
            }
            if ("POST".equals(method) && path.startsWith("/self-check/projection/")
                && path.endsWith("/rebuild")) {
                writeJson(exchange, json, api.rebuildProjection(segment(path, 3)));
                return;
            }
            if ("POST".equals(method) && "/self-check/idempotency/pending".equals(path)) {
                Messages.StartOrderReq request = read(exchange, json, Messages.StartOrderReq.class);
                writeJson(exchange, json, api.createPendingThenStart(request));
                return;
            }
            if ("POST".equals(method) && "/self-check/workflow/inventory-reserved".equals(path)) {
                Messages.StartOrderReq request = read(exchange, json, Messages.StartOrderReq.class);
                writeJson(exchange, json, api.prepareInventoryReserved(request));
                return;
            }
            if ("POST".equals(method) && path.startsWith("/self-check/workflow/")
                && path.endsWith("/continue")) {
                writeJson(exchange, json, api.continueOrder(segment(path, 3)));
                return;
            }
            if ("POST".equals(method) && "/self-check/assert".equals(path)) {
                Messages.ServerAssertionReq request = read(exchange, json, Messages.ServerAssertionReq.class);
                writeJson(exchange, json, 200, api.assertEvidence(request));
                return;
            }
            writeJson(exchange, json, 404, new ErrorBody("not found"));
        } catch (Exception ex) {
            writeJson(exchange, json, 500, new ErrorBody(ex.getMessage()));
        }
    }

    private static void writeJson(
        HttpExchange exchange,
        ObjectMapper json,
        CompletionStage<?> response) {
        response.whenComplete((body, failure) -> {
            try {
                if (failure == null) {
                    writeJson(exchange, json, 200, body);
                    return;
                }
                Throwable cause = failure instanceof CompletionException && failure.getCause() != null
                    ? failure.getCause()
                    : failure;
                writeJson(exchange, json, 500, new ErrorBody(cause.getMessage()));
            } catch (IOException writeFailure) {
                exchange.close();
            }
        });
    }

    private static <T> T read(
        HttpExchange exchange,
        ObjectMapper json,
        Class<T> type) throws IOException {
        try (var body = exchange.getRequestBody()) {
            return json.readValue(body, type);
        }
    }

    private static void writeJson(
        HttpExchange exchange,
        ObjectMapper json,
        int status,
        Object body) throws IOException {
        byte[] bytes = json.writeValueAsString(body).getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("content-type", "application/json");
        exchange.sendResponseHeaders(status, bytes.length);
        exchange.getResponseBody().write(bytes);
        exchange.close();
    }

    private static String last(String path) {
        String[] parts = path.split("/");
        return parts[parts.length - 1];
    }

    private static String segment(String path, int index) {
        String[] parts = path.split("/");
        return parts[index];
    }

    private record Health(String status) {
    }

    private record Ok(boolean ok) {
    }

    private record ErrorBody(String error) {
    }
}
