package systems.zlink.e2e.observabilityops.a5.server;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import org.springframework.context.SmartLifecycle;
import org.springframework.beans.factory.ObjectProvider;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

public final class HttpServer implements SmartLifecycle {
    private final ObjectMapper json;
    private final ObjectProvider<ZLinkFrameworkRuntime> runtimeProvider;
    private final ZLinkRouteClient routes;
    private final FlowEvidence evidence;
    private final Options config;
    private com.sun.net.httpserver.HttpServer server;
    private boolean running;

    public HttpServer(
        ObjectMapper json,
        ObjectProvider<ZLinkFrameworkRuntime> runtimeProvider,
        ZLinkRouteClient routes,
        FlowEvidence evidence,
        Options config) {
        this.json = json;
        this.runtimeProvider = runtimeProvider;
        this.routes = routes;
        this.evidence = evidence;
        this.config = config;
    }

    @Override
    public void start() {
        try {
            URI endpoint = URI.create(config.httpEndpoint());
            server = com.sun.net.httpserver.HttpServer.create(
                new InetSocketAddress(endpoint.getHost(), endpoint.getPort()), 0);
            server.createContext("/health", exchange -> write(exchange, "ok\n"));
            server.createContext("/evidence", this::flowSnapshot);
            server.createContext("/mode", this::setMode);
            server.createContext("/request", this::request);
            server.createContext("/flows", this::flowSnapshot);
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start A5 HTTP server", error);
        }
    }

    private void setMode(HttpExchange exchange) throws java.io.IOException {
        ZLinkMessageFlowLogMode mode = ZLinkMessageFlowLogMode.valueOf(
            query(exchange, "value"));
        runtime().setMessageFlowMode(mode);
        write(exchange, json.writeValueAsString(new Status(
            runtime().messageFlowMode().name(), evidence.snapshot().size())));
    }

    private void request(HttpExchange exchange) throws java.io.IOException {
        try {
            Contracts.ProbeReply reply = routes.requestToChannel(
                    Contracts.CHANNEL,
                    new Contracts.ProbeRequest(
                        query(exchange, "value"),
                        Boolean.parseBoolean(query(exchange, "fail"))))
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.ProbeReply.class)
                .toCompletableFuture()
                .join();
            write(exchange, json.writeValueAsString(reply));
        } catch (Throwable error) {
            write(exchange, 500, failure(error));
        }
    }

    private void flowSnapshot(HttpExchange exchange) throws java.io.IOException {
        int after = Integer.parseInt(queryOrDefault(exchange, "after", "0"));
        List<ZLinkMessageFlowEvent> events = evidence.snapshot();
        int start = Math.max(0, Math.min(after, events.size()));
        write(exchange, json.writeValueAsString(new FlowSnapshot(
            events.size(), events.subList(start, events.size()))));
    }

    private static String query(HttpExchange exchange, String name) {
        String value = queryOrDefault(exchange, name, "");
        if (value.isBlank()) {
            throw new IllegalArgumentException("missing query parameter " + name);
        }
        return value;
    }

    private static String queryOrDefault(HttpExchange exchange, String name, String fallback) {
        String query = exchange.getRequestURI().getRawQuery();
        if (query == null) {
            return fallback;
        }
        for (String part : query.split("&")) {
            String[] pair = part.split("=", 2);
            if (pair.length == 2 && name.equals(pair[0])) {
                return java.net.URLDecoder.decode(pair[1], StandardCharsets.UTF_8);
            }
        }
        return fallback;
    }

    private static String failure(Throwable error) {
        Throwable cause = error.getCause() == null ? error : error.getCause();
        if (cause instanceof ZLinkFrameworkException frameworkError) {
            return frameworkError.kind().name() + ": " + frameworkError.getMessage();
        }
        return cause.getClass().getName() + ": " + cause.getMessage();
    }

    private ZLinkFrameworkRuntime runtime() {
        return runtimeProvider.getObject();
    }

    private static void write(HttpExchange exchange, String body) throws java.io.IOException {
        write(exchange, 200, body);
    }

    private static void write(HttpExchange exchange, int status, String body)
        throws java.io.IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().set("Content-Type", "application/json");
        exchange.sendResponseHeaders(status, bytes.length);
        exchange.getResponseBody().write(bytes);
        exchange.close();
    }

    private record Status(String mode, int count) {
    }

    private record FlowSnapshot(int count, List<ZLinkMessageFlowEvent> events) {
    }

    @Override
    public void stop() {
        if (server != null) {
            server.stop(0);
            server = null;
        }
        running = false;
    }

    @Override
    public boolean isRunning() {
        return running;
    }
}
