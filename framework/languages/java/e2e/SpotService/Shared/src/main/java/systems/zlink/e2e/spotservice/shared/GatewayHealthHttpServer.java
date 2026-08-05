package systems.zlink.e2e.spotservice.shared;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import org.springframework.context.SmartLifecycle;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.monitoring.ZLinkPeerState;

public final class GatewayHealthHttpServer implements SmartLifecycle {
    private static final ObjectMapper JSON = new ObjectMapper();
    private final String endpoint;
    private final ZLinkSpotManager spots;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private HttpServer server;
    private boolean running;

    public GatewayHealthHttpServer(
        String endpoint,
        ZLinkSpotManager spots,
        ZLinkRouteMeshRuntime meshRuntime) {
        this.endpoint = endpoint;
        this.spots = spots;
        this.meshRuntime = meshRuntime;
    }

    @Override
    public void start() {
        if (endpoint == null || endpoint.isBlank()) {
            return;
        }
        try {
            spots.create("gateway-operation")
                .request(new Contracts.CreateSpotReq(
                    "gateway-operations-" + UUID.randomUUID().toString().replace("-", "")))
                .submit()
                .toCompletableFuture().join();
            GatewayOperationSpot operations = GatewayOperationSpot.awaitReady();
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.createContext("/health", exchange -> write(exchange, 200, "ok\n"));
            server.createContext("/topology/ready", exchange -> {
                int expected = Integer.parseInt(queryValue(exchange.getRequestURI(), "expected"));
                long ready = meshRuntime.snapshot(Contracts.SPOT_MESH).peers().stream()
                    .filter(peer -> peer.state() == ZLinkPeerState.READY)
                    .count();
                write(exchange, ready >= expected ? 200 : 503, ready + "\n");
            });
            operation("/operations/spot/state-request", Contracts.SpotStateOperation.class,
                operations::requestState);
            operation("/operations/spot/slow-request", Contracts.SpotStateOperation.class,
                operations::requestSlow);
            operation("/operations/spot/outbound-request", Contracts.SpotValueOperation.class,
                operations::requestOutbound);
            operation("/operations/spot/state-send", Contracts.SpotValueOperation.class,
                operations::sendState);
            operation("/operations/spot/outbound-send", Contracts.SpotValueOperation.class,
                operations::sendOutbound);
            operation("/operations/route/request", Contracts.RouteOperation.class,
                operations::requestRoute);
            operation("/operations/actor/push-request", Contracts.ActorOperation.class,
                operations::requestActorPush);
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start gateway scenario endpoint " + endpoint, error);
        }
    }

    private <T> void operation(String path, Class<T> requestType, Operation<T> operation) {
        server.createContext(path, exchange -> {
            try {
                T request = JSON.readValue(exchange.getRequestBody(), requestType);
                byte[] body = JSON.writeValueAsBytes(operation.execute(request));
                exchange.getResponseHeaders().add("Content-Type", "application/json");
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            } catch (Throwable error) {
                error.printStackTrace(System.err);
                write(exchange, 500, "operation failed: " + error + "\n");
            }
        });
    }

    private static void write(
        com.sun.net.httpserver.HttpExchange exchange,
        int status,
        String value) throws java.io.IOException {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("Content-Type", "text/plain");
        exchange.sendResponseHeaders(status, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    private static String queryValue(URI uri, String key) {
        String query = uri.getRawQuery();
        if (query == null || query.isBlank()) {
            return "";
        }
        for (String part : query.split("&")) {
            String[] fields = part.split("=", 2);
            if (fields.length == 2 && key.equals(fields[0])) {
                return fields[1];
            }
        }
        return "";
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

    @FunctionalInterface
    private interface Operation<T> {
        Object execute(T request);
    }
}
