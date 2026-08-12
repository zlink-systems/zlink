package systems.zlink.e2e.resiliencelifecycle.consumer.endpoints;
import com.sun.net.httpserver.HttpExchange;
import java.io.IOException;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.function.Supplier;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery;
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

public final class ConsumerEndpoints implements SmartLifecycle {
    private final ObjectMapper json;
    private final ZLinkRouteClient routes;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private final ZLinkFrameworkLifecycle lifecycle;
    private final String endpoint;
    private HttpServer server;
    private boolean running;

    public ConsumerEndpoints(
        ObjectMapper json,
        ZLinkRouteClient routes,
        ZLinkRouteMeshRuntime meshRuntime,
        ZLinkFrameworkLifecycle lifecycle,
        String endpoint) {
        this.json = json;
        this.routes = routes;
        this.meshRuntime = meshRuntime;
        this.lifecycle = lifecycle;
        this.endpoint = endpoint;
    }

    @Override
    public void start() {
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.setExecutor(Executors.newCachedThreadPool());
            server.createContext("/health", exchange -> {
                boolean ready;
                try {
                    ready = meshRuntime.snapshot(Contracts.CHANNEL).channels().stream()
                        .anyMatch(channel -> channel.channelName().equals(Contracts.CHANNEL)
                            && channel.isReady());
                } catch (RuntimeException unavailable) {
                    ready = false;
                }
                if (!ready) {
                    writeJson(exchange, 503, Map.of("status", "starting"));
                    return;
                }
                writeJson(exchange, Map.of("status", "ready"));
            });
            server.createContext("/operations/request/work", exchange -> handle(exchange, () -> {
                Contracts.WorkOperation request = read(exchange, Contracts.WorkOperation.class);
                return routes.requestToChannel(Contracts.CHANNEL, new Contracts.WorkReq(request.value()))
                    .timeout(Duration.ofMillis(request.timeoutMillis()))
                    .submit(Contracts.WorkRes.class)
                    .toCompletableFuture()
                    .join();
            }));
            server.createContext("/operations/request/unhandled", exchange -> handle(exchange, () -> {
                Contracts.UnhandledOperation request = read(exchange, Contracts.UnhandledOperation.class);
                return routes.requestToChannel(Contracts.CHANNEL, new Contracts.UnhandledReq(request.value()))
                    .timeout(Duration.ofMillis(request.timeoutMillis()))
                    .submit(Contracts.WorkRes.class)
                    .toCompletableFuture()
                    .join();
            }));
            server.createContext("/operations/send/work", exchange -> handle(exchange, () -> {
                Contracts.WorkMsg request = read(exchange, Contracts.WorkMsg.class);
                routes.sendToChannel(Contracts.CHANNEL, request).submit();
                return Map.of("status", "accepted");
            }));
            server.createContext("/operations/peers", exchange -> handle(exchange, () -> {
                List<Contracts.PeerLocation> peers = lifecycle
                    .monitoringLocationRuntimeQuery()
                    .listTopology(
                        new ZLinkLocationTopologyFilter(Contracts.CHANNEL, null, null),
                        new ZLinkPageRequest(1_000, null))
                    .toCompletableFuture()
                    .join()
                    .items().stream()
                    .map(server -> new Contracts.PeerLocation(
                        server.nodeRid().toString(),
                        server.endpoint(),
                        "",
                        server.updatedAt().toEpochMilli()))
                    .toList();
                return new Contracts.PeerSnapshot(peers);
            }));
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start consumer endpoint " + endpoint, error);
        }
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

    private <T> T read(HttpExchange exchange, Class<T> type) {
        try {
            return json.readValue(exchange.getRequestBody(), type);
        } catch (IOException error) {
            throw new IllegalArgumentException("invalid operation request", error);
        }
    }

    private void handle(
        HttpExchange exchange,
        Supplier<Object> operation) throws IOException {
        try {
            writeJson(exchange, 200, operation.get());
        } catch (RuntimeException error) {
            writeJson(exchange, 500, Map.of("error", error.toString()));
        }
    }

    private void writeJson(HttpExchange exchange, Object value) throws IOException {
        writeJson(exchange, 200, value);
    }

    private void writeJson(
        HttpExchange exchange,
        int status,
        Object value) throws IOException {
        byte[] body = json.writeValueAsBytes(value);
        exchange.getResponseHeaders().add("Content-Type", "application/json");
        exchange.sendResponseHeaders(status, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }
}
