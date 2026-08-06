package systems.zlink.e2e.spotservice.shared;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.context.SmartLifecycle;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

public final class EvidenceHttpServer implements SmartLifecycle {
    private final ScenarioState state;
    private final ObjectMapper json;
    private final String endpoint;
    private final ZLinkSpotManager spots;
    private final ZLinkActorManager actors;
    private final ZLinkActorClient actorClient;
    private final ZLinkRouteClient routes;
    private final ZLinkRouteMeshRuntime meshRuntime;
    private final ZLinkRouteMeshRuntimeOptions meshOptions;
    private final ObjectProvider<ZLinkFrameworkRuntime> frameworkRuntimes;
    private HttpServer server;
    private ExecutorService executor;
    private boolean running;

    public EvidenceHttpServer(
        ScenarioState state,
        ObjectMapper json,
        String endpoint,
        ZLinkSpotManager spots,
        ZLinkActorManager actors,
        ZLinkActorClient actorClient,
        ZLinkRouteClient routes,
        ZLinkRouteMeshRuntime meshRuntime,
        ZLinkRouteMeshRuntimeOptions meshOptions,
        ObjectProvider<ZLinkFrameworkRuntime> frameworkRuntimes) {
        this.state = state;
        this.json = json;
        this.endpoint = endpoint;
        this.spots = spots;
        this.actors = actors;
        this.actorClient = actorClient;
        this.routes = routes;
        this.meshRuntime = meshRuntime;
        this.meshOptions = meshOptions;
        this.frameworkRuntimes = frameworkRuntimes;
    }

    @Override
    public void start() {
        if (endpoint == null || endpoint.isBlank()) {
            return;
        }
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            executor = Executors.newFixedThreadPool(4, runnable -> {
                Thread thread = new Thread(runnable, "evidence-http");
                thread.setDaemon(true);
                return thread;
            });
            server.setExecutor(executor);
            server.createContext("/health", exchange -> {
                byte[] body = "ok\n".getBytes(StandardCharsets.UTF_8);
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            });
            server.createContext("/topology/ready", exchange -> {
                int expected = Integer.parseInt(queryValue(exchange.getRequestURI(), "expected"));
                long ready = meshRuntime.snapshot(Contracts.SPOT_MESH).peers().stream()
                    .filter(peer -> peer.state() == ZLinkPeerState.READY)
                    .count();
                write(exchange, ready >= expected ? 200 : 503, ready + "\n");
            });
            server.createContext("/placement-weight", exchange -> {
                if (!"POST".equalsIgnoreCase(exchange.getRequestMethod())) {
                    write(exchange, 405, "POST required\n");
                    return;
                }
                Contracts.PlacementWeightReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.PlacementWeightReq.class);
                var placement = meshOptions.mesh(Contracts.SPOT_MESH);
                placement.setPlacementWeight(request.weight());
                write(exchange, 200, json.writeValueAsString(
                    new Contracts.PlacementWeightRes(placement.placementWeight())));
            });
            server.createContext("/location/ready", exchange -> {
                boolean ready = meshRuntime.snapshot(Contracts.SPOT_MESH)
                    .placement()
                    .isAvailable();
                write(exchange, ready ? 200 : 503, ready ? "ready\n" : "pending\n");
            });
            server.createContext("/location/spot-owner", exchange -> {
                String spotRid = queryValue(exchange.getRequestURI(), "spotRid");
                var spot = spots.find(spotRid)
                    .toCompletableFuture().join().orElse(null);
                // The public SpotRef exposes the current owner node. The lease owner id
                // remains an internal location detail and is not used by application calls.
                String owner = spot == null ? null : spot.nodeRid().toString();
                write(exchange, owner == null ? 404 : 200,
                    owner == null ? "absent\n" : owner + "\n");
            });
            server.createContext("/entry/identity", exchange ->
                write(exchange, 200, json.writeValueAsString(state.entryIdentity())));
            server.createContext("/spot/a9/start", exchange -> {
                Contracts.GatedSpotCreateReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.GatedSpotCreateReq.class);
                state.armGate(request.spotRid());
                spots.getOrCreate(request.spotRid(), "user")
                    .request("a9")
                    .submit();
                write(exchange, 202, "{\"started\":true}\n");
            });
            server.createContext("/spot/a9/release", exchange -> {
                Contracts.GatedSpotCreateReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.GatedSpotCreateReq.class);
                state.releaseGate(request.spotRid());
                write(exchange, 200, "{\"released\":true}\n");
            });
            server.createContext("/spot/a9/status", exchange -> {
                Contracts.GatedSpotCreateReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.GatedSpotCreateReq.class);
                var result = getOrCreateUserSpot(request.spotRid());
                write(exchange, 200, json.writeValueAsString(new Contracts.CreateSpotRes(
                    result.spot().spotId(), result.spot().nodeRid().toString(), result.state().name())));
            });
            server.createContext("/spot/a9/probe", exchange -> {
                Contracts.GatedSpotCreateReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.GatedSpotCreateReq.class);
                var found = spots.find(request.spotRid()).toCompletableFuture().join();
                if (found.isEmpty()) {
                    write(exchange, 200, json.writeValueAsString(
                        new Contracts.SpotPublicationProbeRes(false, false, null, null, 0)));
                    return;
                }
                var ref = found.orElseThrow();
                try {
                    Contracts.StateRes reply = routes.requestToSpot(
                            request.spotRid(), new Contracts.StateReq("a9-probe"))
                        .timeout(java.time.Duration.ofSeconds(2))
                        .submit(Contracts.StateRes.class).toCompletableFuture().join();
                    write(exchange, 200, json.writeValueAsString(
                        new Contracts.SpotPublicationProbeRes(
                            true, true, ref.nodeRid().toString(), reply.nodeRid(), 1)));
                } catch (RuntimeException error) {
                    write(exchange, 200, json.writeValueAsString(
                        new Contracts.SpotPublicationProbeRes(
                            true, false, ref.nodeRid().toString(), null, 0)));
                }
            });
            server.createContext("/spot/c6/arm", exchange -> {
                Contracts.GatedSpotCreateReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.GatedSpotCreateReq.class);
                state.armGate("c6-delivery:" + request.spotRid());
                write(exchange, 200, "{\"accepted\":true}\n");
            });
            server.createContext("/spot/c6/release", exchange -> {
                Contracts.GatedSpotCreateReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.GatedSpotCreateReq.class);
                state.releaseGate("c6-delivery:" + request.spotRid());
                write(exchange, 200, "{\"accepted\":true}\n");
            });
            server.createContext("/spot/create-automatic-batch", exchange -> {
                Contracts.AutomaticSpotBatchReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.AutomaticSpotBatchReq.class);
                int count = request.count() <= 0 ? 200 : request.count();
                var creates = java.util.stream.IntStream.range(0, count)
                    .mapToObj(index -> java.util.concurrent.CompletableFuture.supplyAsync(() ->
                        spots.create("user").request("automatic-" + index)
                            .submit().toCompletableFuture().join()))
                    .toList();
                var results = creates.stream().map(java.util.concurrent.CompletableFuture::join).toList();
                int successfulRequests = 0;
                for (var result : results) {
                    try {
                        routes.requestToSpot(result.spot().spotId(), new Contracts.StateReq("automatic"))
                            .timeout(java.time.Duration.ofSeconds(5))
                            .submit(Contracts.StateRes.class).toCompletableFuture().join();
                        successfulRequests++;
                    } catch (RuntimeException ignored) {
                    }
                }
                var ids = results.stream().map(result -> result.spot().spotId()).toList();
                write(exchange, 200, json.writeValueAsString(new Contracts.AutomaticSpotBatchRes(
                    count, results.size(), (int) ids.stream().distinct().count(), successfulRequests, ids)));
            });
            server.createContext("/spot/id-boundary", exchange -> {
                String max = "x".repeat(255);
                List<String> ids = List.of("x", max, "Room", "room", "é", "e\u0301");
                List<String> foundIds = new java.util.ArrayList<>();
                List<Integer> stateValues = new java.util.ArrayList<>();
                for (String id : ids) {
                    getOrCreateUserSpot(id);
                    var found = spots.find(id).toCompletableFuture().join().orElseThrow();
                    foundIds.add(found.spotId());
                    routes.requestToSpot(id, new Contracts.StateReq("boundary"))
                        .timeout(java.time.Duration.ofSeconds(5))
                        .submit(Contracts.StateRes.class).toCompletableFuture().join();
                    stateValues.add(1);
                }
                int factoryBefore = countCreated("x".repeat(256));
                String invalidError;
                try {
                    getOrCreateUserSpot("x".repeat(256));
                    invalidError = "none";
                } catch (RuntimeException error) {
                    invalidError = error.getCause() == null
                        ? error.getClass().getSimpleName()
                        : error.getCause().getClass().getSimpleName();
                }
                int factoryAfter = countCreated("x".repeat(256));
                write(exchange, 200, json.writeValueAsString(new Contracts.SpotIdBoundaryRes(
                    ids, foundIds, stateValues, ids.equals(foundIds), invalidError,
                    factoryAfter - factoryBefore)));
            });
            server.createContext("/spot/reserved-entry-probe", exchange -> {
                String entrySpotId = state.entryIdentity().entrySpotId();
                int before = countCreated(entrySpotId);
                String errorKind;
                try {
                    getOrCreateUserSpot(entrySpotId);
                    errorKind = "none";
                } catch (RuntimeException error) {
                    errorKind = errorKind(error);
                }
                int after = countCreated(entrySpotId);
                write(exchange, 200, json.writeValueAsString(new Contracts.ReservedEntryProbeRes(
                    entrySpotId, errorKind, after - before)));
            });
            server.createContext("/actor/manager-probe", exchange -> {
                Contracts.ActorManagerProbeReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.ActorManagerProbeReq.class);
                var result = switch (request.operation()) {
                    case "find" -> actors.find(request.actorId()).thenApply(found ->
                        new Contracts.ActorManagerProbeRes(
                            found.isPresent() ? "Found" : "Missing",
                            found.map(EvidenceHttpServer::actorView).orElse(null), null));
                    case "create" -> actors.create(request.actorId(), "scenario")
                        .request(new Contracts.ActorAuthReq(
                            request.actorId(), new Contracts.ActorProfile("manager", 1, List.of("manager"))))
                        .submit().thenApply(EvidenceHttpServer::actorResult);
                    case "get-or-create" -> actors.getOrCreate(request.actorId(), "scenario")
                        .request(new Contracts.ActorAuthReq(
                            request.actorId(), new Contracts.ActorProfile("manager", 1, List.of("manager"))))
                        .submit().thenApply(EvidenceHttpServer::actorResult);
                    default -> throw new IllegalArgumentException("unknown actor manager operation");
                };
                write(exchange, 200, json.writeValueAsString(result.toCompletableFuture().join()));
            });
            server.createContext("/actor/create-race", exchange -> {
                Contracts.ActorCreateRaceReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.ActorCreateRaceReq.class);
                var first = actors.getOrCreate(request.actorId(), "scenario")
                    .request(new Contracts.ActorAuthReq(request.actorId(),
                        new Contracts.ActorProfile("reject:first", 1, List.of("reject"))))
                    .submit().toCompletableFuture().join();
                var second = actors.getOrCreate(request.actorId(), "scenario")
                    .request(new Contracts.ActorAuthReq(request.actorId(),
                        new Contracts.ActorProfile("accepted:second", 1, List.of("accepted"))))
                    .submit().toCompletableFuture().join();
                var finalRef = actors.find(request.actorId()).toCompletableFuture().join().orElseThrow();
                String firstReply = first instanceof ZLinkActorCreateResult.Rejected rejected
                    ? rejected.reply().decode(String.class) : "";
                ActorRef secondRef = actorResultRef(second);
                write(exchange, 200, json.writeValueAsString(new Contracts.ActorCreateRaceRes(
                    first instanceof ZLinkActorCreateResult.Rejected ? "Rejected" : "Other",
                    firstReply,
                    second instanceof ZLinkActorCreateResult.Created ? "Created" : "Other",
                    actorView(secondRef), actorView(finalRef))));
            });
            server.createContext("/actor/b11/start", exchange -> {
                Contracts.ActorRefReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.ActorRefReq.class);
                state.armGate(request.actorId());
                actors.create(request.actorId(), "scenario")
                    .request(new Contracts.ActorAuthReq(request.actorId(),
                        new Contracts.ActorProfile("sm-b11-gated", 1, List.of("gated"))))
                    .submit();
                write(exchange, 202, "{\"started\":true}\n");
            });
            server.createContext("/actor/b11/release", exchange -> {
                Contracts.ActorRefReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.ActorRefReq.class);
                state.releaseGate(request.actorId());
                write(exchange, 200, "{\"released\":true}\n");
            });
            server.createContext("/actor/request", exchange -> {
                Contracts.ActorRequestReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.ActorRequestReq.class);
                try {
                    var reply = actorClient.requestToActor(request.actorId(),
                            new Contracts.ActorPingReq(request.value()))
                        .timeout(java.time.Duration.ofMillis(request.timeoutMilliseconds()))
                        .submit(Contracts.ActorPingRes.class).toCompletableFuture().join();
                    write(exchange, 200, json.writeValueAsString(new Contracts.ActorRequestRes(
                        true, reply.actorId(), reply.value(), null)));
                } catch (RuntimeException error) {
                    write(exchange, 200, json.writeValueAsString(new Contracts.ActorRequestRes(
                        false, request.actorId(), null, errorKind(error))));
                }
            });
            server.createContext("/placement/probe", exchange -> {
                Contracts.PlacementWeightReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.PlacementWeightReq.class);
                var placement = meshOptions.mesh(Contracts.SPOT_MESH);
                try {
                    placement.setPlacementWeight(request.weight());
                    write(exchange, 200, json.writeValueAsString(
                        new Contracts.PlacementProbeRes(true, placement.placementWeight(), null)));
                } catch (RuntimeException error) {
                    write(exchange, 200, json.writeValueAsString(
                        new Contracts.PlacementProbeRes(false, placement.placementWeight(), errorKind(error))));
                }
            });
            server.createContext("/placement/batch", exchange -> {
                Contracts.PlacementBatchReq request = json.readValue(
                    exchange.getRequestBody(), Contracts.PlacementBatchReq.class);
                int count = request.count() <= 0 ? 800 : request.count();
                int actorA = 0;
                int actorB = 0;
                for (int start = 0; start < count; start += 16) {
                    List<java.util.concurrent.CompletableFuture<ZLinkActorCreateResult>> creates =
                        java.util.stream.IntStream.range(start, Math.min(start + 16, count))
                            .mapToObj(index -> {
                                String actorId = "sm-g5-actor-" + request.suffix() + "-" + index;
                                return actors.create(actorId, "scenario")
                                    .request(new Contracts.ActorAuthReq(actorId,
                                        new Contracts.ActorProfile("g5", 1, List.of())))
                                    .submit()
                                    .toCompletableFuture();
                            })
                            .toList();
                    java.util.concurrent.CompletableFuture.allOf(
                        creates.toArray(java.util.concurrent.CompletableFuture[]::new)).join();
                    for (var create : creates) {
                        if (actorView(actorResultRef(create.join())).nodeRid().equals("play-a")) {
                            actorA++;
                        } else {
                            actorB++;
                        }
                    }
                }
                write(exchange, 200, json.writeValueAsString(new Contracts.PlacementBatchRes(
                    count, actorA, actorB, 0, 0, 0)));
            });
            server.createContext("/placement/capacity", exchange -> {
                String suffix = queryValue(exchange.getRequestURI(), "suffix");
                String first = "sm-g5-capacity-first-" + suffix;
                String second = "sm-g5-capacity-second-" + suffix;
                // Make the first request eligible only on the remote node;
                // after that placement, the local node is the only candidate
                // with remaining stable-type capacity.
                meshOptions.mesh(Contracts.SPOT_MESH).setPlacementWeight(0);
                var firstResult = spots.getOrCreate(first, "capacity").request("capacity")
                    .submit().toCompletableFuture().join();
                meshOptions.mesh(Contracts.SPOT_MESH).setPlacementWeight(10_000);
                var secondResult = spots.getOrCreate(second, "capacity").request("capacity")
                    .submit().toCompletableFuture().join();
                write(exchange, 200, json.writeValueAsString(new Contracts.CapacityPlacementRes(
                    firstResult.spot().nodeRid().toString(), secondResult.spot().nodeRid().toString())));
            });
            server.createContext("/admin/relocate", exchange -> {
                if (!"POST".equalsIgnoreCase(exchange.getRequestMethod())) {
                    write(exchange, 405, "POST required\n");
                    return;
                }
                try {
                    var result = frameworkRuntimes.getObject().relocate(
                            new ZLinkFrameworkRelocationOptions(
                                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                null,
                                java.time.Duration.ofSeconds(30)))
                        .toCompletableFuture()
                        .join();
                    write(exchange, 200, json.writeValueAsString(
                        new Contracts.RelocationRes(
                            result.outcome().name(), result.reason().name())));
                } catch (RuntimeException error) {
                    write(exchange, 500, failureText(error));
                }
            });
            server.createContext("/evidence", exchange -> {
                byte[] body = json.writeValueAsBytes(state.snapshot());
                exchange.getResponseHeaders().add("Content-Type", "application/json");
                exchange.sendResponseHeaders(200, body.length);
                exchange.getResponseBody().write(body);
                exchange.close();
            });
            server.createContext("/evidence/wait", exchange -> {
                try {
                    Contracts.EvidenceWaitReq request = json.readValue(
                        exchange.getRequestBody(),
                        Contracts.EvidenceWaitReq.class);
                    int timeout = request.timeoutMilliseconds() <= 0
                        ? 10_000
                        : request.timeoutMilliseconds();
                    byte[] body = json.writeValueAsBytes(state.waitFor(request.containsAll(), timeout));
                    exchange.getResponseHeaders().add("Content-Type", "application/json");
                    exchange.sendResponseHeaders(200, body.length);
                    exchange.getResponseBody().write(body);
                    exchange.close();
                } catch (RuntimeException error) {
                    write(exchange, 500, error.getMessage() + "\n");
                }
            });
            server.createContext("/spot/create", exchange -> {
                Contracts.CreateSpotReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.CreateSpotReq.class);
                var result = getOrCreateUserSpot(request.spotRid());
                write(exchange, 200, json.writeValueAsString(new Contracts.CreateSpotRes(
                    result.spot().spotId(),
                    result.spot().nodeRid().toString(),
                    result.state().name())));
            });
            server.createContext("/spot/state/request", exchange -> {
                try {
                    Contracts.SpotStateRouteReq request = json.readValue(
                        exchange.getRequestBody(),
                        Contracts.SpotStateRouteReq.class);
                    Contracts.StateRes result = requestStateWithRetry(request);
                    write(exchange, 200, json.writeValueAsString(result));
                } catch (RuntimeException error) {
                    write(exchange, 500, failureText(error));
                }
            });
            server.createContext("/spot/missing-handler/request", exchange -> {
                Contracts.SpotMissingHandlerReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.SpotMissingHandlerReq.class);
                boolean failed = fails(() -> routes.requestToSpot(
                        request.spotRid(),
                        new Contracts.MissingSpotReq("noop"))
                    .timeout(java.time.Duration.ofSeconds(2))
                    .submit(Contracts.StateRes.class).toCompletableFuture().join());
                write(exchange, 200, json.writeValueAsString(new Contracts.SpotMissingHandlerRes(
                    request.spotRid(),
                    failed,
                    state.snapshot())));
            });
            server.createContext("/spot/missing-handler/command", exchange -> {
                Contracts.SpotMissingCommandReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.SpotMissingCommandReq.class);
                try {
                    routes.sendToSpot(
                            request.spotRid(),
                            new Contracts.MissingSpotMsg(request.marker()))
                        .submit();
                } catch (RuntimeException ignored) {
                    // The assertion below is the contract: a missing send handler is recorded as a drop.
                }
                write(exchange, 200, json.writeValueAsString(new Contracts.SpotMissingCommandRes(
                    request.spotRid(),
                    request.marker(),
                    true,
                    state.snapshot())));
            });
            server.createContext("/spot/stage/request", exchange -> {
                Contracts.SpotStageProbeRouteReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.SpotStageProbeRouteReq.class);
                Contracts.StateRes result = requestStageWithRetry(request);
                write(exchange, 200, json.writeValueAsString(result));
            });
            server.createContext("/spot/stage/timer", exchange -> {
                Contracts.SpotStageTimerRouteReq request = json.readValue(
                    exchange.getRequestBody(),
                    Contracts.SpotStageTimerRouteReq.class);
                Contracts.StageTimerStartRes result = requestStageTimerWithRetry(request);
                write(exchange, 200, json.writeValueAsString(result));
            });
            server.createContext("/admin/close", exchange -> {
                String rid = queryValue(exchange.getRequestURI(), "rid");
                if (rid == null || rid.isBlank()) {
                    write(exchange, 400, "missing rid\n");
                    return;
                }
                boolean closed = closeSpot(rid);
                write(exchange, 200, "{\"closed\":" + closed + "}\n");
            });
            server.createContext("/admin/create-timer", exchange -> {
                String rid = queryValue(exchange.getRequestURI(), "rid");
                if (rid == null || rid.isBlank()) {
                    write(exchange, 400, "missing rid\n");
                    return;
                }
                try {
                    spots.getOrCreate(rid, localSpotType("timer"))
                        .request("e2e")
                        .submit()
                        .toCompletableFuture()
                        .get(5, java.util.concurrent.TimeUnit.SECONDS);
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("timer spot create interrupted", error);
                } catch (java.util.concurrent.ExecutionException
                         | java.util.concurrent.TimeoutException error) {
                    throw new IllegalStateException("timer spot create failed", error);
                }
                write(exchange, 200, "{\"created\":true}\n");
            });
            server.createContext("/admin/type-mismatch", exchange -> {
                String rid = queryValue(exchange.getRequestURI(), "rid");
                if (rid == null || rid.isBlank()) {
                    write(exchange, 400, "missing rid\n");
                    return;
                }
                try {
                    spots.getOrCreate(rid, localSpotType("mismatched"))
                        .submit()
                        .toCompletableFuture()
                        .get(5, java.util.concurrent.TimeUnit.SECONDS);
                    write(exchange, 500, "{\"mismatch\":false}\n");
                    return;
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("spot type mismatch interrupted", error);
                } catch (java.util.concurrent.ExecutionException error) {
                    state.record("SpotTypeMismatch", rid, error.getCause().getMessage());
                } catch (java.util.concurrent.TimeoutException error) {
                    throw new IllegalStateException("spot type mismatch timed out", error);
                } catch (RuntimeException error) {
                    state.record("SpotTypeMismatch", rid, error.getMessage());
                }
                try {
                    spots.getOrCreate(rid, localSpotType("user"))
                        .submit()
                        .toCompletableFuture()
                        .get(5, java.util.concurrent.TimeUnit.SECONDS);
                    state.record("SpotTypeMismatchStateOk", rid, "existing");
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("spot type mismatch follow-up interrupted", error);
                } catch (java.util.concurrent.ExecutionException
                         | java.util.concurrent.TimeoutException error) {
                    throw new IllegalStateException("spot type mismatch follow-up failed", error);
                }
                write(exchange, 200, "{\"mismatch\":true}\n");
            });
            server.start();
            running = true;
        } catch (Exception error) {
            if (executor != null) {
                executor.shutdownNow();
                executor = null;
            }
            throw new IllegalStateException("failed to start evidence endpoint " + endpoint, error);
        }
    }

    private systems.zlink.framework.spots.ZLinkSpotCreateResult getOrCreateUserSpot(String spotRid) {
        try {
            return spots.getOrCreate(spotRid, localSpotType("user"))
                .request("e2e")
                .submit()
                .toCompletableFuture()
                .get(5, java.util.concurrent.TimeUnit.SECONDS);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("spot create interrupted", error);
        } catch (java.util.concurrent.ExecutionException
                 | java.util.concurrent.TimeoutException error) {
            throw new IllegalStateException("spot create failed", error);
        }
    }

    private String localSpotType(String family) {
        return family;
    }

    private Contracts.StateRes requestStateWithRetry(Contracts.SpotStateRouteReq request) {
        long deadline = System.nanoTime() + java.time.Duration.ofSeconds(10).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                String op = request.op() + "-" + request.delta();
                return routes.requestToSpot(
                        request.spotRid(),
                        new Contracts.StateReq(op))
                    .timeout(java.time.Duration.ofSeconds(2))
                    .submit(Contracts.StateRes.class).toCompletableFuture().join();
            } catch (RuntimeException error) {
                lastFailure = error;
                try {
                    Thread.sleep(100);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("interrupted while waiting for spot state route", interrupted);
                }
            }
        }
        throw new IllegalStateException(
            "timed out waiting for spot state route " + request.spotRid(),
            lastFailure);
    }

    private Contracts.StateRes requestStageWithRetry(Contracts.SpotStageProbeRouteReq request) {
        return requestToSpotWithRetry(
            request.spotRid(),
            new Contracts.StageProbeReq(request.marker(), request.delta()),
            "StageProbeReq",
            Contracts.StateRes.class,
            "spot stage route");
    }

    private Contracts.StageTimerStartRes requestStageTimerWithRetry(Contracts.SpotStageTimerRouteReq request) {
        return requestToSpotWithRetry(
            request.spotRid(),
            new Contracts.StageTimerStartReq(request.name(), request.periodMilliseconds()),
            "StageTimerStartReq",
            Contracts.StageTimerStartRes.class,
            "spot stage timer route");
    }

    private <T> T requestToSpotWithRetry(
        String spotRid,
        Object packet,
        String packetName,
        Class<T> responseType,
        String operation) {
        long deadline = System.nanoTime() + java.time.Duration.ofSeconds(10).toNanos();
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return routes.requestToSpot(
                        spotRid,
                        packet)
                    .timeout(java.time.Duration.ofSeconds(2))
                    .submit(responseType).toCompletableFuture().join();
            } catch (RuntimeException error) {
                lastFailure = error;
                try {
                    Thread.sleep(100);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("interrupted while waiting for " + operation, interrupted);
                }
            }
        }
        throw new IllegalStateException(
            "timed out waiting for " + operation + " " + spotRid,
            lastFailure);
    }

    private static boolean fails(Runnable action) {
        try {
            action.run();
            return false;
        } catch (RuntimeException error) {
            return true;
        }
    }

    private boolean closeSpot(String spotRid) {
        return spots.find(spotRid)
            .thenCompose(found -> found
                .map(spots::close)
                .orElseGet(() -> java.util.concurrent.CompletableFuture.completedFuture(false)))
            .toCompletableFuture()
            .join();
    }

    private int countCreated(String spotId) {
        return (int) state.snapshot().entries().stream()
            .filter(entry -> "SpotCreated".equals(entry.marker())
                && spotId.equals(entry.spotRid()))
            .count();
    }

    private static Contracts.ActorManagerProbeRes actorResult(ZLinkActorCreateResult result) {
        if (result instanceof ZLinkActorCreateResult.Created created) {
            return new Contracts.ActorManagerProbeRes("Created", actorView(created.actor()),
                created.reply() == null ? null : created.reply().decode(String.class));
        }
        if (result instanceof ZLinkActorCreateResult.Existing existing) {
            return new Contracts.ActorManagerProbeRes("Existing", actorView(existing.actor()), null);
        }
        if (result instanceof ZLinkActorCreateResult.Rejected rejected) {
            return new Contracts.ActorManagerProbeRes("Rejected", null,
                rejected.reply() == null ? null : rejected.reply().decode(String.class));
        }
        throw new IllegalStateException("unknown actor create result");
    }

    private static ActorRef actorResultRef(ZLinkActorCreateResult result) {
        if (result instanceof ZLinkActorCreateResult.Created created) return created.actor();
        if (result instanceof ZLinkActorCreateResult.Existing existing) return existing.actor();
        throw new IllegalStateException("actor creation was rejected");
    }

    private static Contracts.ActorRefView actorView(ActorRef actor) {
        return new Contracts.ActorRefView(
            actor.actorId(), actor.objectGeneration(), actor.nodeRid().toString());
    }

    private static String errorKind(Throwable error) {
        Throwable current = error;
        while (current.getCause() != null) current = current.getCause();
        return current.getClass().getSimpleName();
    }

    private static String queryValue(URI uri, String name) {
        String query = uri.getRawQuery();
        if (query == null || query.isBlank()) {
            return null;
        }
        for (String part : query.split("&")) {
            String[] pair = part.split("=", 2);
            if (pair.length == 2 && name.equals(pair[0])) {
                return java.net.URLDecoder.decode(pair[1], StandardCharsets.UTF_8);
            }
        }
        return null;
    }

    private static void write(
        com.sun.net.httpserver.HttpExchange exchange,
        int status,
        String value) throws java.io.IOException {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
        exchange.sendResponseHeaders(status, body.length);
        exchange.getResponseBody().write(body);
        exchange.close();
    }

    private static String failureText(Throwable error) {
        StringBuilder text = new StringBuilder();
        Throwable current = error;
        while (current != null) {
            if (!text.isEmpty()) {
                text.append(" caused by ");
            }
            text.append(current.getClass().getSimpleName()).append(": ")
                .append(current.getMessage());
            current = current.getCause();
        }
        return text.append('\n').toString();
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
