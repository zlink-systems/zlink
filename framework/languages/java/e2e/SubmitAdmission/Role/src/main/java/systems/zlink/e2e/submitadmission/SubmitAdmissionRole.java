package systems.zlink.e2e.submitadmission;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.springframework.boot.WebApplicationType;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.builder.SpringApplicationBuilder;
import org.springframework.context.annotation.Bean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
public class SubmitAdmissionRole {
    private static final String MESH = "submit.mesh";
    private static final String CHANNEL = "submit.channel";
    private static final String FANOUT = "submit.fanout";
    private static RoleConfig roleConfig;

    public static void main(String[] args) {
        roleConfig = RoleConfig.parse(args);
        SpringApplicationBuilder builder = new SpringApplicationBuilder(SubmitAdmissionRole.class)
            .web(WebApplicationType.NONE)
            .properties("spring.main.banner-mode=off", "logging.level.root=ERROR");
        builder.application().setKeepAlive(true);
        builder.run(args);
    }

    @Bean
    RoleState roleState() {
        return new RoleState(roleConfig);
    }

    @Bean
    RouteHandler routeHandler(RoleState state) {
        return new RouteHandler(state);
    }

    @Bean
    ChannelHandler channelHandler(RoleState state) {
        return new ChannelHandler(state);
    }

    @Bean
    FanoutHandler fanoutHandler(RoleState state) {
        return new FanoutHandler(state);
    }

    @Bean
    ZLinkFrameworkConfigurer configureFramework(RoleState state) {
        return options -> {
            RoleConfig config = state.config();
            if (!"publisher".equals(config.role()) && !"subscriber".equals(config.role())) {
                var mesh = options.addRouteMesh(MESH)
                    .listen(config.meshEndpoint())
                    .setRoutingId(RoutingId.from(config.rid()))
                    .setDefaultRequestTimeout(Duration.ofSeconds(1));
                mesh.addRouteSendHandler(RouteHandler.class, Probe.class);
                mesh.channelName(CHANNEL)
                    .server()
                    .setWeight("target".equals(config.role()) ? 100 : 0)
                    .addSendHandler(ChannelHandler.class, Probe.class);
                if (!config.peerEndpoint().isBlank()) {
                    mesh.peerConnections().connect(
                        RoutingId.from(config.peerRid()), config.peerEndpoint());
                }
            }
            if ("publisher".equals(config.role())) {
                options.addFanoutChannel(FANOUT)
                    .enablePublisher(config.fanoutEndpoint());
            } else if ("subscriber".equals(config.role())) {
                options.addHandlersFromPackageOf(FanoutHandler.class);
                options.addFanoutChannel(FANOUT)
                    .connect(config.fanoutEndpoint())
                    .addPublishHandler(FanoutHandler.class, Probe.class);
            }
        };
    }

    @Bean(destroyMethod = "close")
    RoleHttpServer roleHttpServer(
        RoleState state,
        ZLinkRouteClient routes,
        ZLinkFanoutClient fanout,
        ZLinkRouteMeshRuntime meshRuntime) throws IOException {
        return new RoleHttpServer(state, routes, fanout, meshRuntime);
    }

    public record Probe(String operationId, int sequence) {
    }

    public static final class RouteHandler implements ZLinkRouteSendHandler<Probe> {
        private final RoleState state;

        public RouteHandler(RoleState state) {
            this.state = state;
        }

        @Override
        public CompletionStage<Void> handle(Probe message, ZLinkRouteMessageContext context) {
            return state.handle("route", message);
        }
    }

    public static final class FanoutHandler implements ZLinkFanoutHandler<Probe> {
        private final RoleState state;

        public FanoutHandler(RoleState state) {
            this.state = state;
        }

        @Override
        public CompletionStage<Void> handle(Probe message, ZLinkPublishMessageContext context) {
            return state.handle("fanout", message);
        }
    }

    public static final class ChannelHandler implements ZLinkSendHandler<Probe> {
        private final RoleState state;

        public ChannelHandler(RoleState state) {
            this.state = state;
        }

        @Override
        public CompletionStage<Void> handle(Probe message, ZLinkMessageContext context) {
            return state.handle("channel", message);
        }
    }

    static final class RoleState {
        private final RoleConfig config;
        private final AtomicInteger handlerStarted = new AtomicInteger();
        private final AtomicInteger handlerCompleted = new AtomicInteger();

        RoleState(RoleConfig config) {
            this.config = config;
        }

        RoleConfig config() {
            return config;
        }

        CompletionStage<Void> handle(String family, Probe message) {
            int started = handlerStarted.incrementAndGet();
            record("handler_started", Map.of(
                "family", family,
                "operationId", message.operationId(),
                "sequence", Integer.toString(message.sequence()),
                "handlerStarted", Integer.toString(started)));
            CompletableFuture<Void> completion = new CompletableFuture<>();
            Thread.ofVirtual().start(() -> {
                try {
                    if (!config.gateFile().isBlank()
                        && message.operationId().startsWith("handler-gate")) {
                        Path gate = Path.of(config.gateFile());
                        while (!Files.exists(gate)) {
                            Thread.sleep(10L);
                        }
                    }
                    int completed = handlerCompleted.incrementAndGet();
                    record("handler_completed", Map.of(
                        "family", family,
                        "operationId", message.operationId(),
                        "sequence", Integer.toString(message.sequence()),
                        "handlerCompleted", Integer.toString(completed)));
                    completion.complete(null);
                } catch (Throwable failure) {
                    completion.completeExceptionally(failure);
                }
            });
            return completion;
        }

        synchronized void record(String event, Map<String, String> fields) {
            if (config.evidenceFile().isBlank()) {
                return;
            }
            StringBuilder line = new StringBuilder();
            line.append("{\"timestamp\":\"").append(Instant.now()).append("\"")
                .append(",\"role\":\"").append(escape(config.role())).append("\"")
                .append(",\"event\":\"").append(escape(event)).append("\"");
            fields.entrySet().stream().sorted(Map.Entry.comparingByKey()).forEach(entry ->
                line.append(",\"").append(escape(entry.getKey())).append("\":\"")
                    .append(escape(entry.getValue())).append("\""));
            line.append("}\n");
            try {
                Files.writeString(
                    Path.of(config.evidenceFile()), line,
                    StandardCharsets.UTF_8,
                    java.nio.file.StandardOpenOption.CREATE,
                    java.nio.file.StandardOpenOption.APPEND);
            } catch (IOException failure) {
                throw new IllegalStateException("failed to write evidence", failure);
            }
        }

        int handlerStarted() {
            return handlerStarted.get();
        }

        int handlerCompleted() {
            return handlerCompleted.get();
        }

        private static String escape(String value) {
            return value.replace("\\", "\\\\").replace("\"", "\\\"");
        }
    }

    static final class RoleHttpServer implements AutoCloseable {
        private final RoleState state;
        private final ZLinkRouteClient routes;
        private final ZLinkFanoutClient fanout;
        private final ZLinkRouteMeshRuntime meshRuntime;
        private final HttpServer server;

        RoleHttpServer(
            RoleState state,
            ZLinkRouteClient routes,
            ZLinkFanoutClient fanout,
            ZLinkRouteMeshRuntime meshRuntime) throws IOException {
            this.state = state;
            this.routes = routes;
            this.fanout = fanout;
            this.meshRuntime = meshRuntime;
            server = HttpServer.create(new InetSocketAddress("127.0.0.1", state.config().httpPort()), 0);
            server.setExecutor(Executors.newVirtualThreadPerTaskExecutor());
            server.createContext("/health", exchange -> respond(exchange, 200, "ok"));
            server.createContext("/ready", this::ready);
            server.createContext("/send-node", this::sendNode);
            server.createContext("/send-channel", this::sendChannel);
            server.createContext("/publish", this::publish);
            server.createContext("/counts", this::counts);
            server.start();
            state.record("role_ready", Map.of("httpPort", Integer.toString(state.config().httpPort())));
        }

        private void ready(HttpExchange exchange) throws IOException {
            String target = query(exchange.getRequestURI()).getOrDefault("targetRid", "");
            boolean ready = false;
            if (!"publisher".equals(state.config().role()) && !"subscriber".equals(state.config().role())) {
                ready = meshRuntime.snapshot(MESH).peers().stream()
                    .anyMatch(peer -> (peer.state() == ZLinkPeerState.READY
                        || peer.state() == ZLinkPeerState.NOT_REQUIRED)
                        && peer.nodeRid().toString().equals(target));
            }
            respond(exchange, ready ? 200 : 503, Boolean.toString(ready));
        }

        private void sendNode(HttpExchange exchange) throws IOException {
            Map<String, String> query = query(exchange.getRequestURI());
            String operationId = query.getOrDefault("operationId", "missing-operation");
            String targetRid = query.getOrDefault("targetRid", "missing-target");
            int sequence = Integer.parseInt(query.getOrDefault("sequence", "0"));
            try {
                routes.sendToNode(
                        MESH, RoutingId.from(targetRid), new Probe(operationId, sequence))
                    .submit().toCompletableFuture().get(2, TimeUnit.SECONDS);
                state.record("submit_terminal", Map.of(
                    "family", "node",
                    "operationId", operationId,
                    "status", "Submitted"));
                respond(exchange, 200, "Submitted");
            } catch (Exception failure) {
                respond(exchange, 500, errorKind(failure));
            }
        }

        private void sendChannel(HttpExchange exchange) throws IOException {
            Map<String, String> query = query(exchange.getRequestURI());
            String operationId = query.getOrDefault("operationId", "missing-operation");
            int sequence = Integer.parseInt(query.getOrDefault("sequence", "0"));
            try {
                routes.sendToChannel(CHANNEL, new Probe(operationId, sequence))
                    .submit().toCompletableFuture().get(2, TimeUnit.SECONDS);
                state.record("submit_terminal", Map.of(
                    "family", "channel",
                    "operationId", operationId,
                    "status", "Submitted"));
                respond(exchange, 200, "Submitted");
            } catch (Exception failure) {
                respond(exchange, 500, errorKind(failure));
            }
        }

        private void publish(HttpExchange exchange) throws IOException {
            Map<String, String> query = query(exchange.getRequestURI());
            String operationId = query.getOrDefault("operationId", "missing-operation");
            int sequence = Integer.parseInt(query.getOrDefault("sequence", "0"));
            try {
                fanout.publish(FANOUT, new Probe(operationId, sequence))
                    .submit().toCompletableFuture().get(2, TimeUnit.SECONDS);
                state.record("submit_terminal", Map.of(
                    "family", "fanout",
                    "operationId", operationId,
                    "status", "Submitted"));
                respond(exchange, 200, "Submitted");
            } catch (Exception failure) {
                respond(exchange, 500, errorKind(failure));
            }
        }

        private void counts(HttpExchange exchange) throws IOException {
            respond(exchange, 200,
                "started=" + state.handlerStarted() + ",completed=" + state.handlerCompleted());
        }

        @Override
        public void close() {
            server.stop(0);
        }

        private static Map<String, String> query(URI uri) {
            Map<String, String> values = new HashMap<>();
            String raw = uri.getRawQuery();
            if (raw == null || raw.isBlank()) {
                return values;
            }
            for (String item : raw.split("&")) {
                String[] pair = item.split("=", 2);
                values.put(
                    java.net.URLDecoder.decode(pair[0], StandardCharsets.UTF_8),
                    pair.length == 1 ? "" : java.net.URLDecoder.decode(pair[1], StandardCharsets.UTF_8));
            }
            return values;
        }

        private static void respond(HttpExchange exchange, int status, String body) throws IOException {
            byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
            exchange.sendResponseHeaders(status, bytes.length);
            try (var output = exchange.getResponseBody()) {
                output.write(bytes);
            }
        }

        private static String errorKind(Throwable failure) {
            Throwable current = failure;
            while ((current instanceof CompletionException || current instanceof ExecutionException)
                && current.getCause() != null) {
                current = current.getCause();
            }
            return current instanceof ZLinkFrameworkException frameworkError
                ? frameworkError.kind().name()
                : current.toString();
        }
    }

    record RoleConfig(
        String role,
        String rid,
        int httpPort,
        String meshEndpoint,
        String peerRid,
        String peerEndpoint,
        String fanoutEndpoint,
        String gateFile,
        String evidenceFile) {

        static RoleConfig parse(String[] args) {
            Map<String, String> values = new HashMap<>();
            for (String argument : args) {
                if (!argument.startsWith("--") || !argument.contains("=")) {
                    continue;
                }
                String[] pair = argument.substring(2).split("=", 2);
                values.put(pair[0], pair[1]);
            }
            return new RoleConfig(
                required(values, "role"),
                required(values, "rid"),
                Integer.parseInt(required(values, "httpPort")),
                values.getOrDefault("meshEndpoint", ""),
                values.getOrDefault("peerRid", ""),
                values.getOrDefault("peerEndpoint", ""),
                values.getOrDefault("fanoutEndpoint", ""),
                values.getOrDefault("gateFile", ""),
                values.getOrDefault("evidenceFile", ""));
        }

        private static String required(Map<String, String> values, String key) {
            String value = values.get(key);
            if (value == null || value.isBlank()) {
                throw new IllegalArgumentException("--" + key + " is required");
            }
            return value;
        }
    }
}
