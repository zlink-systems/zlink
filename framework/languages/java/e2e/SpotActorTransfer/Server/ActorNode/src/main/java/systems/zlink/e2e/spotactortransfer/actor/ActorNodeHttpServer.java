package systems.zlink.e2e.spotactortransfer.actor;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.TimeUnit;
import org.springframework.context.SmartLifecycle;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.spotactortransfer.shared.Contracts;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpotManager;

public final class ActorNodeHttpServer implements SmartLifecycle {
    private final String endpoint;
    private final ObjectMapper json;
    private final EvidenceStore evidence;
    private final GateStore gates;
    private final ZLinkSpotManager spots;
    private final ZLinkActorManager actors;
    private final ZLinkActorClient actorClient;
    private final int requiredPeerCount;
    private final systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle
        lifecycle;
    private HttpServer server;
    private java.util.concurrent.ExecutorService executor;
    private boolean running;

    public ActorNodeHttpServer(
        String endpoint,
        ObjectMapper json,
        EvidenceStore evidence,
        GateStore gates,
        ZLinkSpotManager spots,
        ZLinkActorManager actors,
        ZLinkActorClient actorClient,
        int requiredPeerCount,
        systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle lifecycle) {
        this.endpoint = endpoint;
        this.json = json;
        this.evidence = evidence;
        this.gates = gates;
        this.spots = spots;
        this.actors = actors;
        this.actorClient = actorClient;
        this.requiredPeerCount = requiredPeerCount;
        this.lifecycle = lifecycle;
    }

    @Override
    public void start() {
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            executor = java.util.concurrent.Executors.newCachedThreadPool();
            server.setExecutor(executor);
            server.createContext("/health", exchange -> handle(exchange, () -> {
                var mesh = lifecycle.routeMeshRuntime().snapshot(Contracts.MESH);
                if (!lifecycle.isReady()
                    || !mesh.isReady()
                    || mesh.readyPeerCount() < requiredPeerCount) {
                    writeText(
                        exchange,
                        503,
                        "Framework runtime state="
                            + lifecycle.status().state().name());
                    return;
                }
                writeJson(exchange, java.util.Map.of(
                    "status", "ready",
                    "nodeRid", evidence.nodeRid()));
            }));
            server.createContext("/evidence", exchange -> handle(exchange, () ->
                writeJson(exchange, evidence.snapshot())));
            server.createContext("/spots", exchange -> handle(exchange, () -> createSpot(exchange)));
            server.createContext("/gates", exchange -> handle(exchange, () -> releaseGate(exchange)));
            server.createContext("/actors", exchange -> handle(exchange, () -> actors(exchange)));
            server.createContext("/runtime/retire", exchange -> handle(
                exchange, () -> retire(exchange)));
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start actor node HTTP endpoint " + endpoint, error);
        }
    }

    private void retire(HttpExchange exchange) throws Exception {
        var relocation = lifecycle.relocate(
                new systems.zlink.framework.runtime.host
                    .ZLinkFrameworkRelocationOptions(
                        systems.zlink.framework.runtime.host
                            .ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                        null,
                        Duration.ofSeconds(30)))
            .toCompletableFuture()
            .get(35, TimeUnit.SECONDS);
        if (relocation.outcome()
            != systems.zlink.framework.runtime.host
                .ZLinkFrameworkRelocationOutcome.RELOCATED) {
            writeJson(exchange, new Contracts.RetireRes(
                "RELOCATE_THEN_SHUTDOWN",
                relocation.outcome().name(),
                relocation.reason().name()));
            return;
        }
        var termination = lifecycle.shutdown(Duration.ofSeconds(30))
            .toCompletableFuture()
            .get(35, TimeUnit.SECONDS);
        writeJson(exchange, new Contracts.RetireRes(
            "RELOCATE_THEN_SHUTDOWN",
            termination.outcome().name(),
            termination.reason().name()));
    }

    private void createSpot(HttpExchange exchange) throws Exception {
        Contracts.CreateSpotReq request = json.readValue(
            exchange.getRequestBody(), Contracts.CreateSpotReq.class);
        var result = spots.getOrCreate(
                request.spotRid(),
                TransferComponents.TransferUserSpot.class.getName())
            .request(ZLinkMessage.of(request))
            .timeout(Duration.ofSeconds(5))
            .submit()
            .toCompletableFuture()
            .get(12, TimeUnit.SECONDS);
        writeJson(exchange, new Contracts.CreateSpotRes(
            result.spot().spotId(),
            result.spot().nodeRid().toString(),
            result.state().name()));
    }

    private void releaseGate(HttpExchange exchange) throws Exception {
        String[] path = pathParts(exchange);
        if (path.length != 2) {
            throw new IllegalArgumentException("gate path must be /gates/{key}");
        }
        writeJson(exchange, new Contracts.GateReleaseRes(path[1], gates.release(path[1])));
    }

    private void actors(HttpExchange exchange) throws Exception {
        String[] path = pathParts(exchange);
        if (path.length == 1) {
            createActor(exchange);
            return;
        }
        if (path.length != 3) {
            throw new IllegalArgumentException("actor path must identify an operation");
        }
        switch (path[2]) {
            case "join" -> joinActor(exchange, path[1]);
            case "probe" -> probeActor(exchange, path[1]);
            case "probe-timeout" -> probeActorWithTimeout(exchange, path[1]);
            case "probe-ref" -> probeActorRef(exchange, path[1]);
            case "send-ref" -> sendActorRef(exchange, path[1]);
            case "ref" -> actorRef(exchange, path[1]);
            default -> throw new IllegalArgumentException("unknown actor operation: " + path[2]);
        }
    }

    private void createActor(HttpExchange exchange) throws Exception {
        Contracts.ActorCreateReq request = json.readValue(
            exchange.getRequestBody(), Contracts.ActorCreateReq.class);
        ZLinkActorCreateResult result = actors.getOrCreate(
                request.actorId(),
                request.actorType())
            .request(request)
            .submit()
            .toCompletableFuture()
            .get(12, TimeUnit.SECONDS);
        ActorRef actor = switch (result) {
            case ZLinkActorCreateResult.Created created -> created.actor();
            case ZLinkActorCreateResult.Existing existing -> existing.actor();
            case ZLinkActorCreateResult.Rejected rejected ->
                throw new IllegalStateException("Actor creation was rejected");
        };
        writeJson(exchange, new Contracts.ActorCreateRes(
            actor.actorId(), request.actorType(), actor.nodeRid().toString(),
            actor.objectGeneration()));
    }

    private void joinActor(HttpExchange exchange, String actorId) throws Exception {
        Contracts.JoinTargetReq request = json.readValue(
            exchange.getRequestBody(), Contracts.JoinTargetReq.class);
        try {
            Contracts.JoinTargetRes result = actorClient
                .requestToActor(actorId, request)
                .timeout(Duration.ofSeconds(12))
                .submit(Contracts.JoinTargetRes.class).toCompletableFuture().join();
            if (result.accepted()) {
                var target = spots.find(request.targetSpotRid())
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS)
                    .orElseThrow(() -> new IllegalStateException(
                        "target Spot was not found: "
                            + request.targetSpotRid()));
                Optional<ActorRef> resolved =
                    awaitActorJoinOutcome(actorId, target.nodeRid());
                if (resolved.isEmpty()) {
                    result = new Contracts.JoinTargetRes(
                        result.scenario(),
                        result.actorId(),
                        false,
                        result.sourceNodeRid(),
                        result.targetSpotRid(),
                        result.stateVersion());
                } else {
                    ActorRef committed = resolved.orElseThrow();
                    evidence.add(
                        request.scenario(), actorId, "location_visible",
                        committed.nodeRid() + ":"
                            + Long.toUnsignedString(
                                committed.objectGeneration()));
                }
            }
            evidence.add(request.scenario(), actorId,
                result.accepted() ? "success_reply" : "reject_reply", request.targetSpotRid());
            writeJson(exchange, result);
        } catch (RuntimeException error) {
            evidence.add(request.scenario(), actorId, "join_failed", rootMessage(error));
            writeJson(exchange, new Contracts.JoinTargetRes(
                request.scenario(), actorId, false, evidence.nodeRid(),
                request.targetSpotRid(), 0));
        }
    }

    private Optional<ActorRef> awaitActorJoinOutcome(
        String actorId,
        RoutingId expectedOwner) throws Exception {
        long deadline = System.nanoTime()
            + Duration.ofSeconds(12).toNanos();
        while (System.nanoTime() < deadline) {
            Optional<ActorRef> actor = actors.find(actorId)
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
            if (actor.isPresent()
                && actor.orElseThrow().nodeRid().equals(expectedOwner)) {
                return actor;
            }
            boolean failed = evidence.snapshot().entries().stream()
                .anyMatch(entry -> actorId.equals(entry.actorId())
                    && ("join_failed".equals(entry.kind())
                        || "join_rejected".equals(entry.kind())));
            if (failed) {
                return Optional.empty();
            }
            Thread.sleep(20);
        }
        throw new java.util.concurrent.TimeoutException(
            "Actor location did not commit to target owner");
    }

    private void probeActor(HttpExchange exchange, String actorId) throws Exception {
        Contracts.ProbeReq request = json.readValue(
            exchange.getRequestBody(), Contracts.ProbeReq.class);
        Contracts.ProbeRes result = actorClient
            .requestToActor(actorId, request)
            .timeout(Duration.ofSeconds(10))
            .submit(Contracts.ProbeRes.class).toCompletableFuture().join();
        writeJson(exchange, result);
    }

    private void probeActorWithTimeout(HttpExchange exchange, String actorId) throws Exception {
        Contracts.ProbeWithTimeoutReq request = json.readValue(
            exchange.getRequestBody(), Contracts.ProbeWithTimeoutReq.class);
        try {
            Contracts.ProbeRes result = actorClient
                .requestToActor(
                    actorId,
                    new Contracts.ProbeReq(request.scenario(), request.marker()))
                .timeout(Duration.ofMillis(request.timeoutMillis()))
                .submit(Contracts.ProbeRes.class).toCompletableFuture().join();
            writeJson(exchange, result);
        } catch (RuntimeException error) {
            evidence.add(request.scenario(), actorId, "request_timeout", errorKind(error));
            throw error;
        }
    }

    private void actorRef(HttpExchange exchange, String actorId) throws Exception {
        ActorRef actor = requireActor(actorId);
        writeJson(exchange, java.util.Map.of(
            "actorId", actor.actorId(),
            "nodeRid", actor.nodeRid().toString(),
            "objectGeneration", actor.objectGeneration()));
    }

    private void probeActorRef(HttpExchange exchange, String actorId) throws Exception {
        Contracts.ProbeAtRefReq request = json.readValue(
            exchange.getRequestBody(), Contracts.ProbeAtRefReq.class);
        throw new UnsupportedOperationException(
            "ST-F4/F5 require a transport delivery-delay fixture; public Actor messaging "
                + "does not accept an owner route.");
    }

    private void sendActorRef(HttpExchange exchange, String actorId) throws Exception {
        Contracts.SendAtRefReq request = json.readValue(
            exchange.getRequestBody(), Contracts.SendAtRefReq.class);
        throw new UnsupportedOperationException(
            "ST-F4/F5 require a transport delivery-delay fixture; public Actor messaging "
                + "does not accept an owner route.");
    }

    private ActorRef requireActor(String actorId) throws Exception {
        Optional<ActorRef> actor = actors.find(actorId).toCompletableFuture().get(5, TimeUnit.SECONDS);
        return actor.orElseThrow(() -> new IllegalStateException("actor was not found: " + actorId));
    }

    private static String[] pathParts(HttpExchange exchange) {
        return java.util.Arrays.stream(exchange.getRequestURI().getPath().split("/"))
            .filter(part -> !part.isBlank())
            .toArray(String[]::new);
    }

    private void handle(HttpExchange exchange, ThrowingRunnable action) throws java.io.IOException {
        try {
            action.run();
        } catch (Exception error) {
            error.printStackTrace(System.err);
            writeText(exchange, 500, rootMessage(error));
        }
    }

    private void writeJson(HttpExchange exchange, Object value) throws java.io.IOException {
        byte[] body = json.writeValueAsBytes(value);
        exchange.getResponseHeaders().add("Content-Type", "application/json");
        exchange.sendResponseHeaders(200, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    private static void writeText(HttpExchange exchange, int status, String value)
        throws java.io.IOException {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(status, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    private static String rootMessage(Throwable error) {
        Throwable current = error;
        while (current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof systems.zlink.contracts.errors.ZlinkRequestException request) {
            return request.getClass().getName()
                + ": result=" + request.getResult()
                + ", errno=" + request.getNativeErrno();
        }
        if (current instanceof systems.zlink.contracts.errors.ZlinkSubmitException submit) {
            return submit.getClass().getName()
                + ": result=" + submit.getResult()
                + ", errno=" + submit.getNativeErrno();
        }
        return current.toString();
    }

    private static String errorKind(Throwable error) {
        Throwable current = error;
        while (current instanceof java.util.concurrent.CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current instanceof ZLinkFrameworkException frameworkError
            ? frameworkError.kind().name()
            : rootMessage(current);
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

    @FunctionalInterface
    private interface ThrowingRunnable {
        void run() throws Exception;
    }
}
