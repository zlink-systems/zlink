package systems.zlink.e2e.spotservice.multinode;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import org.springframework.context.SmartLifecycle;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.e2e.spotservice.shared.MultiNodeSpot;
import systems.zlink.e2e.spotservice.shared.ScenarioState;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.spots.ZLinkSpotManager;

public final class MultiNodeHttpServer implements SmartLifecycle {
    private final ScenarioState state;
    private final ObjectMapper json;
    private final String endpoint;
    private final ZLinkSpotManager spots;
    private final ZLinkRouteClient routes;
    private final ZLinkActorManager actors;
    private final ZLinkActorClient actorClient;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private HttpServer server;
    private boolean running;

    public MultiNodeHttpServer(
        ScenarioState state,
        ObjectMapper json,
        String endpoint,
        ZLinkSpotManager spots,
        ZLinkRouteClient routes,
        ZLinkActorManager actors,
        ZLinkActorClient actorClient,
        ZLinkRouteMeshRuntime meshRuntime) {
        this.state = state;
        this.json = json;
        this.endpoint = endpoint;
        this.spots = spots;
        this.routes = routes;
        this.actors = actors;
        this.actorClient = actorClient;
        this.meshRuntime = meshRuntime;
    }

    @Override
    public void start() {
        if (endpoint == null || endpoint.isBlank()) {
            return;
        }
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.createContext("/health", exchange -> handle(exchange, () -> writeJson(exchange, new Health("ready", state.nodeRid()))));
            server.createContext("/topology/ready", exchange -> {
                int expected = Integer.parseInt(queryValue(exchange.getRequestURI(), "expected"));
                long ready = meshRuntime.snapshot(Contracts.SPOT_MESH).peers().stream()
                    .filter(peer -> peer.state() == ZLinkPeerState.READY)
                    .count();
                if (ready < expected) {
                    writeText(exchange, 503, ready + "\n");
                    return;
                }
                writeJson(exchange, java.util.Map.of("ready", ready));
            });
            server.createContext("/evidence", exchange -> handle(exchange, () -> writeJson(exchange, state.snapshot())));
            server.createContext("/evidence/wait", exchange -> handle(exchange, () -> {
                Contracts.EvidenceWaitReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.EvidenceWaitReq.class);
                int timeout = request.timeoutMilliseconds() <= 0 ? 10_000 : request.timeoutMilliseconds();
                writeJson(exchange, state.waitFor(request.containsAll(), timeout));
            }));
            server.createContext("/spot/create-local", exchange -> handle(exchange, () -> {
                Contracts.MultiNodeCreateSpotReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.MultiNodeCreateSpotReq.class);
                var result = getLocalSpot(request.spotRid());
                state.record("MultiNodeCreateSpot", request.spotRid(), result.state().name());
                writeJson(exchange, new Contracts.MultiNodeCreateSpotRes(
                    result.spot().spotId(),
                    state.nodeRid(),
                    result.state().name(),
                    0));
            }));
            server.createContext("/spot/create-user-local", exchange -> handle(exchange, () -> {
                Contracts.CreateSpotReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.CreateSpotReq.class);
                var result = getLocalSpot(request.spotRid());
                state.record("SpotOnlyCreate", request.spotRid(), result.state().name());
                writeJson(exchange, new Contracts.CreateSpotRes(
                    result.spot().spotId(),
                    state.nodeRid(),
                    result.state().name()));
            }));
            server.createContext("/spot/spot-only/request-send", exchange -> handle(exchange, () -> {
                Contracts.SpotOnlyMeshReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.SpotOnlyMeshReq.class);
                var result = getLocalSpot(request.sourceSpotRid(), ZLinkMessage.of(request));
                Contracts.EvidenceSnapshot evidence = state.waitFor(
                    java.util.List.of(
                        "SpotOnlyRequest|" + state.nodeRid() + "|" + request.sourceSpotRid()
                            + "|" + request.targetSpotRid() + "/7/" + request.marker()),
                    10_000);
                writeJson(exchange, new Contracts.SpotOnlyMeshRes(
                    result.spot().spotId(),
                    request.targetSpotRid(),
                    extractSpotOnlyValue(evidence, request),
                    request.marker()));
            }));
            server.createContext("/actor/spot-only-join", exchange -> handle(exchange, () -> {
                Contracts.SpotOnlyJoinReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.SpotOnlyJoinReq.class);
                var actor = actors.getOrCreate(
                        request.actorId(),
                        "scenario")
                    .request(new Contracts.ActorAuthReq(
                            request.actorId(),
                            new Contracts.ActorProfile("Spot Only", 6, java.util.List.of("spot-only"))))
                    .submit()
                    .thenApply(result -> {
                        if (result instanceof systems.zlink.framework.actors.ZLinkActorCreateResult.Existing existing) {
                            return existing.actor();
                        }
                        if (result instanceof systems.zlink.framework.actors.ZLinkActorCreateResult.Created created) {
                            return created.actor();
                        }
                        throw new IllegalStateException("spot-only actor creation was rejected");
                    })
                    .toCompletableFuture()
                    .get(5, java.util.concurrent.TimeUnit.SECONDS);
                Contracts.SpotOnlyJoinRes result = actorClient
                    .requestToActor(actor.actorId(), request)
                    .timeout(java.time.Duration.ofSeconds(10))
                    .submit(Contracts.SpotOnlyJoinRes.class).toCompletableFuture().join();
                state.waitFor(
                    java.util.List.of(
                        "SpotOnlyActorJoin|" + state.nodeRid() + "|entry|" + request.actorId()
                            + "/" + request.targetSpotRid() + "/" + result.accepted() + "/" + request.marker()),
                    10_000);
                writeJson(exchange, result);
            }));
            server.createContext("/spot/state/request", exchange -> handle(exchange, () -> {
                Contracts.MultiNodeStateRouteReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.MultiNodeStateRouteReq.class);
                Contracts.MultiNodeStateRes result = requestStateWithRetry(request);
                writeJson(exchange, result);
            }));
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start multi-node endpoint " + endpoint, error);
        }
    }

    private systems.zlink.framework.spots.ZLinkSpotCreateResult getLocalSpot(String spotRid) {
        return getLocalSpot(spotRid, ZLinkMessage.empty());
    }

    private systems.zlink.framework.spots.ZLinkSpotCreateResult getLocalSpot(
        String spotRid,
        ZLinkMessage request) {
        try {
            return spots.getOrCreate(spotRid, "multi-node")
                .request(request)
                .submit()
                .toCompletableFuture()
                .get(5, java.util.concurrent.TimeUnit.SECONDS);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("multi-node spot create interrupted", error);
        } catch (java.util.concurrent.ExecutionException
                 | java.util.concurrent.TimeoutException error) {
            throw new IllegalStateException("multi-node spot create failed", error);
        }
    }

    private static int extractSpotOnlyValue(
        Contracts.EvidenceSnapshot evidence,
        Contracts.SpotOnlyMeshReq request) {
        String suffix = request.targetSpotRid() + "/7/" + request.marker();
        return evidence.entries().stream()
            .filter(entry -> "SpotOnlyRequest".equals(entry.marker())
                && request.sourceSpotRid().equals(entry.spotRid())
                && suffix.equals(entry.value()))
            .findFirst()
            .map(entry -> 7)
            .orElse(0);
    }

    private Contracts.MultiNodeStateRes requestStateWithRetry(
        Contracts.MultiNodeStateRouteReq request) {
        long deadline = System.nanoTime() + java.time.Duration.ofSeconds(10).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return routes.requestToSpot(
                        request.spotRid(),
                        new Contracts.MultiNodeStateReq(request.delta()))
                    .timeout(java.time.Duration.ofSeconds(2))
                    .submit(Contracts.MultiNodeStateRes.class).toCompletableFuture().join();
            } catch (RuntimeException error) {
                lastFailure = error;
                try {
                    Thread.sleep(100);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("interrupted while waiting for multi-node spot route", interrupted);
                }
            }
        }
        throw new IllegalStateException(
            "timed out waiting for multi-node spot route " + request.spotRid(),
            lastFailure);
    }

    private void handle(
        com.sun.net.httpserver.HttpExchange exchange,
        ThrowingRunnable action) throws java.io.IOException {
        try {
            action.run();
        } catch (Exception error) {
            System.err.println("multi-node HTTP handler failed: " + exchange.getRequestURI());
            error.printStackTrace(System.err);
            writeText(exchange, 500, error.toString());
        }
    }

    private void writeJson(
        com.sun.net.httpserver.HttpExchange exchange,
        Object value) throws java.io.IOException {
        byte[] body = json.writeValueAsBytes(value);
        exchange.getResponseHeaders().add("Content-Type", "application/json");
        exchange.sendResponseHeaders(200, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    private static void writeText(
        com.sun.net.httpserver.HttpExchange exchange,
        int status,
        String value) throws java.io.IOException {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(status, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
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

    private record Health(String status, String rid) {
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


    @FunctionalInterface
    private interface ThrowingRunnable {
        void run() throws Exception;
    }
}
