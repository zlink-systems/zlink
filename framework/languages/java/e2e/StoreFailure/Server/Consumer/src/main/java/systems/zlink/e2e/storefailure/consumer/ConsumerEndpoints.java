package systems.zlink.e2e.storefailure.consumer;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.storefailure.shared.Contracts;
import systems.zlink.e2e.storefailure.shared.HttpSupport;
import systems.zlink.e2e.storefailure.shared.Wait;
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery;
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

public final class ConsumerEndpoints implements SmartLifecycle {
    private final ConsumerOptions options;
    private final ZLinkClient client;
    private final ZLinkRouteClient routes;
    private final ZLinkFrameworkLifecycle lifecycle;
    private final java.util.function.Supplier<ZLinkLocationRuntimeQuery> locationQuery;
    private final LocationStoreDelayState delayState;
    private final ObjectMapper json;
    private HttpServer server;
    private ExecutorService executor;
    private boolean running;

    public ConsumerEndpoints(
        ConsumerOptions options,
        ZLinkClient client,
        ZLinkRouteClient routes,
        ZLinkFrameworkLifecycle lifecycle,
        java.util.function.Supplier<ZLinkLocationRuntimeQuery> locationQuery,
        LocationStoreDelayState delayState,
        ObjectMapper json) {
        this.options = options;
        this.client = client;
        this.routes = routes;
        this.lifecycle = lifecycle;
        this.locationQuery = locationQuery;
        this.delayState = delayState;
        this.json = json;
    }

    @Override
    public void start() {
        try {
            server = HttpSupport.createServer(options.httpEndpoint());
            executor = Executors.newFixedThreadPool(8);
            server.setExecutor(executor);
            server.createContext("/health", exchange -> HttpSupport.writeJson(
                exchange,
                json,
                java.util.Map.of("status", "ready", "rid", options.rid())));
            server.createContext("/profile/request", exchange -> {
                Contracts.ProfileReq request =
                    HttpSupport.readJson(exchange, json, Contracts.ProfileReq.class);
                HttpSupport.writeJson(exchange, json, request(request));
            });
            server.createContext("/instance/request", exchange -> {
                Contracts.InstanceReq request =
                    HttpSupport.readJson(exchange, json, Contracts.InstanceReq.class);
                HttpSupport.writeJson(exchange, json, instanceRequest(request));
            });
            server.createContext("/profile/request/wait", exchange -> {
                //  Handler가 예외를 그대로 흘리면 응답이 나가지 않아 호출자에게는
                //  무응답(hang)으로 보인다. 실패도 반드시 응답으로 만든다.
                //  안쪽 catch가 원인을 버리면 그 응답조차 "timed out"만 말하므로
                //  마지막 오류를 잡아 두었다가 함께 돌려준다.
                java.util.concurrent.atomic.AtomicReference<Exception> lastError =
                    new java.util.concurrent.atomic.AtomicReference<>();
                try {
                    Contracts.ProfileReq request =
                        HttpSupport.readJson(exchange, json, Contracts.ProfileReq.class);
                    Contracts.ProfileRes reply = Wait.until(
                        Duration.ofSeconds(10),
                        "timed out waiting for profile request routing",
                        () -> {
                            try {
                                return request(request);
                            } catch (Exception error) {
                                lastError.set(error);
                                return null;
                            }
                        });
                    HttpSupport.writeJson(exchange, json, reply);
                } catch (RuntimeException error) {
                    Exception cause = lastError.get();
                    String body = "{\"error\":\""
                        + String.valueOf(error.getMessage()).replace('"', '\'')
                        + " lastCause="
                        + (cause == null ? "none" : cause.toString().replace('"', '\''))
                        + "\"}";
                    byte[] payload = body.getBytes(java.nio.charset.StandardCharsets.UTF_8);
                    exchange.getResponseHeaders().add("Content-Type", "application/json");
                    exchange.sendResponseHeaders(500, payload.length);
                    try (java.io.OutputStream out = exchange.getResponseBody()) {
                        out.write(payload);
                    }
                }
            });
            server.createContext("/locations/status", exchange -> HttpSupport.writeJson(
                exchange,
                json,
                status()));
            server.createContext("/locations/peers", exchange -> HttpSupport.writeJson(
                exchange,
                json,
                peers()));
            server.createContext("/mesh/peers", exchange -> HttpSupport.writeJson(
                exchange,
                json,
                meshPeers()));
            server.createContext("/c4/client-probe", exchange -> {
                Contracts.C4ProbeReq request =
                    HttpSupport.readJson(exchange, json, Contracts.C4ProbeReq.class);
                HttpSupport.writeJson(exchange, json, c4ClientProbe(request));
            });
            server.createContext("/admin/store-delay", exchange -> {
                Contracts.StoreDelayReq request =
                    HttpSupport.readJson(exchange, json, Contracts.StoreDelayReq.class);
                delayState.setDelay(Duration.ofMillis(request.delayMilliseconds()));
                HttpSupport.writeJson(
                    exchange,
                    json,
                    java.util.Map.of("delayMilliseconds", delayState.delayMilliseconds()));
            });
            server.createContext("/shutdown", exchange -> {
                HttpSupport.writeJson(exchange, json, java.util.Map.of("status", "stopping"));
                HttpSupport.shutdownAsync();
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException(
                "failed to start consumer endpoints " + options.httpEndpoint(), error);
        }
    }

    private Contracts.ProfileRes request(Contracts.ProfileReq request) {
        return client.requestToChannel(Contracts.CHANNEL, request)
            .timeout(Duration.ofSeconds(3))
            .submit(Contracts.ProfileRes.class)
            .toCompletableFuture()
            .join();
    }

    private Contracts.InstanceOutcome instanceRequest(Contracts.InstanceReq request) {
        try {
            Contracts.InstanceRes reply = routes
                .requestToSpot(request.spotId(), request)
                .timeout(Duration.ofSeconds(3))
                .instanceSpot(Contracts.LEASE_PROBE_SPOT_TYPE)
                .inMesh(Contracts.CHANNEL)
                .submit(Contracts.InstanceRes.class)
                .toCompletableFuture()
                .join();
            return new Contracts.InstanceOutcome(true, reply, "", "");
        } catch (RuntimeException error) {
            Throwable cause = unwrap(error);
            String kind = cause instanceof ZLinkFrameworkException frameworkError
                ? frameworkError.kind().name()
                : cause.getClass().getSimpleName();
            return new Contracts.InstanceOutcome(
                false,
                null,
                kind,
                cause.getMessage() == null ? "" : cause.getMessage());
        }
    }

    private Contracts.C4RolesRes c4ClientProbe(Contracts.C4ProbeReq request) {
        if (!options.c4Roles()) {
            throw new IllegalStateException("C4 client role is not configured");
        }
        String marker = request.marker();
        Contracts.ProfileRes routeA = routes
            .requestToChannel(
                Contracts.C4_ROUTE_A_CHANNEL,
                new Contracts.ProfileReq("c4-route-a-" + marker, "c4-route-a-" + marker))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.ProfileRes.class)
            .toCompletableFuture()
            .join();
        Contracts.ProfileRes routeB = routes
            .requestToChannel(
                Contracts.C4_ROUTE_B_CHANNEL,
                new Contracts.ProfileReq("c4-route-b-" + marker, "c4-route-b-" + marker))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.ProfileRes.class)
            .toCompletableFuture()
            .join();
        Contracts.ProfileRes clientServer = client
            .requestToChannel(
                Contracts.C4_CLIENT_SERVER_CHANNEL,
                new Contracts.ProfileReq("c4-client-" + marker, "c4-client-" + marker))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.ProfileRes.class)
            .toCompletableFuture()
            .join();
        return new Contracts.C4RolesRes(
            routeA.providerRid(),
            routeA.providerLifecycle(),
            routeB.providerRid(),
            routeB.providerLifecycle(),
            clientServer.providerRid(),
            clientServer.providerLifecycle(),
            marker);
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while ((current instanceof java.util.concurrent.CompletionException
                || current instanceof java.util.concurrent.ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private List<java.util.Map<String, Object>> peers() {
        //  Peer 목록은 store를 직접 열거해서 얻지 않는다. 참조 lane인 `.NET`
        //  consumer도 IZLinkLocationRuntimeQuery.ListTopologyAsync를 쓴다.
        return locationQuery.get().listTopology(
                ZLinkLocationTopologyFilter.all(),
                new ZLinkPageRequest(1_000, null))
            .toCompletableFuture()
            .join()
            .items().stream()
            .map(peer -> java.util.Map.<String, Object>of(
                "nodeRid", peer.nodeRid().toString(),
                "endpoint", peer.endpoint(),
                "ownerId", "",
                "role", "ROUTER",
                "state", peer.state().name(),
                "draining", peer.draining(),
                "meshName", peer.meshName()))
            .toList();
    }

    private List<java.util.Map<String, Object>> meshPeers() {
        return lifecycle.routeMeshRuntime().snapshot(Contracts.CHANNEL).peers().stream()
            .map(peer -> java.util.Map.<String, Object>of(
                "nodeRid", peer.nodeRid().toString(),
                "state", peer.state().name()))
            .toList();
    }

    private java.util.Map<String, Object> status() {
        var status = lifecycle.monitoringLocationRuntimeQuery().getStatus().toCompletableFuture().join();
        return java.util.Map.of(
            "storeHealthy", status.storeHealthy(),
            "watchEnabled", status.watchEnabled(),
            "pollingIntervalMillis", status.pollingInterval().toMillis(),
            "lastRefreshAt", status.lastRefreshAt() == null ? "" : status.lastRefreshAt().toString(),
            "lastError", status.lastError() == null ? "" : status.lastError(),
            "ownerLeaseHealthy", status.ownerLeaseHealthy(),
            "ownerLeaseRenewedAt", status.ownerLeaseRenewedAt() == null ? "" : status.ownerLeaseRenewedAt().toString());
    }

    @Override
    public void stop() {
        if (server != null) {
            server.stop(0);
            server = null;
        }
        if (executor != null) {
            executor.shutdownNow();
            executor = null;
        }
        running = false;
    }

    @Override
    public boolean isRunning() {
        return running;
    }
}
