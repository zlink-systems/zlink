package systems.zlink.e2e.resiliencelifecycle.consumer.endpoints;

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
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

public final class ConsumerEndpoints implements SmartLifecycle {
    private final ObjectMapper json;
    private final ZLinkClient client;
    private final ZLinkFrameworkLifecycle lifecycle;
    private final ZLinkLocationStore locations;
    private final String endpoint;
    private HttpServer server;
    private boolean running;

    public ConsumerEndpoints(
        ObjectMapper json,
        ZLinkClient client,
        ZLinkFrameworkLifecycle lifecycle,
        ZLinkLocationStore locations,
        String endpoint) {
        this.json = json;
        this.client = client;
        this.lifecycle = lifecycle;
        this.locations = locations;
        this.endpoint = endpoint;
    }

    @Override
    public void start() {
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.setExecutor(Executors.newCachedThreadPool());
            server.createContext("/health", exchange -> writeJson(exchange, Map.of("status", "ready")));
            server.createContext("/operations/request/work", exchange -> handle(exchange, () -> {
                Contracts.WorkOperation request = read(exchange, Contracts.WorkOperation.class);
                return client.requestToChannel(Contracts.CHANNEL, new Contracts.WorkReq(request.value()))
                    .timeout(Duration.ofMillis(request.timeoutMillis()))
                    .submit(Contracts.WorkRes.class)
                    .toCompletableFuture()
                    .join();
            }));
            server.createContext("/operations/request/unhandled", exchange -> handle(exchange, () -> {
                Contracts.UnhandledOperation request = read(exchange, Contracts.UnhandledOperation.class);
                return client.requestToChannel(Contracts.CHANNEL, new Contracts.UnhandledReq(request.value()))
                    .timeout(Duration.ofMillis(request.timeoutMillis()))
                    .submit(Contracts.WorkRes.class)
                    .toCompletableFuture()
                    .join();
            }));
            server.createContext("/operations/send/work", exchange -> handle(exchange, () -> {
                Contracts.WorkMsg request = read(exchange, Contracts.WorkMsg.class);
                client.sendToChannel(Contracts.CHANNEL, request).submit();
                return Map.of("status", "accepted");
            }));
            server.createContext("/operations/peers", exchange -> handle(exchange, () -> {
                List<Contracts.PeerLocation> peers = locations
                    .listClientServers(
                        Contracts.CHANNEL,
                        new ZLinkPageRequest(1_000, null))
                    .toCompletableFuture()
                    .join()
                    .items().stream()
                    .map(server -> new Contracts.PeerLocation(
                        server.serverRid().toString(),
                        server.endpoint(),
                        server.ownerId(),
                        server.lifecycleGeneration()))
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

    private <T> T read(com.sun.net.httpserver.HttpExchange exchange, Class<T> type) {
        try {
            return json.readValue(exchange.getRequestBody(), type);
        } catch (java.io.IOException error) {
            throw new IllegalArgumentException("invalid operation request", error);
        }
    }

    private void handle(
        com.sun.net.httpserver.HttpExchange exchange,
        Supplier<Object> operation) throws java.io.IOException {
        try {
            writeJson(exchange, 200, operation.get());
        } catch (RuntimeException error) {
            writeJson(exchange, 500, Map.of("error", error.toString()));
        }
    }

    private void writeJson(com.sun.net.httpserver.HttpExchange exchange, Object value) throws java.io.IOException {
        writeJson(exchange, 200, value);
    }

    private void writeJson(
        com.sun.net.httpserver.HttpExchange exchange,
        int status,
        Object value) throws java.io.IOException {
        byte[] body = json.writeValueAsBytes(value);
        exchange.getResponseHeaders().add("Content-Type", "application/json");
        exchange.sendResponseHeaders(status, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }
}
