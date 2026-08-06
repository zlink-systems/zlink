package systems.zlink.e2e.observabilityops.play;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import io.micrometer.core.instrument.Meter;
import io.micrometer.core.instrument.MeterRegistry;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.context.SmartLifecycle;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.messaging.ZLinkMessage;

/** HTTP adapter for the public host runtime used by Config 11 E2E scenarios. */
public final class MaintenanceHttpServer implements SmartLifecycle {
    private final ObjectMapper json;
    private final MeterRegistry metrics;
    private final ObjectProvider<ZLinkFrameworkRuntime> runtimeProvider;
    private final ZLinkSpotManager spots;
    private final PlayOptions config;
    private final MaintenanceGate gate;
    private final String endpoint;
    private HttpServer server;
    private volatile boolean running;

    public MaintenanceHttpServer(
        ObjectMapper json,
        MeterRegistry metrics,
        ObjectProvider<ZLinkFrameworkRuntime> runtimeProvider,
        ZLinkSpotManager spots,
        PlayOptions config,
        MaintenanceGate gate) {
        this.json = json;
        this.metrics = metrics;
        this.runtimeProvider = runtimeProvider;
        this.spots = spots;
        this.config = config;
        this.gate = gate;
        this.endpoint = config.maintenanceEndpoint();
    }

    @Override
    public void start() {
        if (endpoint.isBlank()) {
            return;
        }
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.setExecutor(Executors.newCachedThreadPool(runnable -> {
                Thread thread = new Thread(runnable, "observability-maintenance-http");
                thread.setDaemon(true);
                return thread;
            }));
            server.createContext("/maintenance/health", exchange -> write(exchange, Map.of("ready", true)));
            server.createContext("/maintenance/status", exchange -> write(exchange, status()));
            server.createContext("/maintenance/objects", exchange -> write(exchange, objects(exchange)));
            server.createContext("/maintenance/metrics", exchange -> write(exchange, metrics()));
            server.createContext("/maintenance/relocate", exchange -> write(exchange, relocate(exchange)));
            server.createContext("/maintenance/shutdown", exchange -> write(exchange, shutdown(exchange)));
            server.createContext("/maintenance/spot/create", exchange -> write(exchange, createSpot(exchange)));
            server.createContext("/maintenance/spot/close", exchange -> write(exchange, closeSpot(exchange)));
            server.createContext("/maintenance/gate/arm", exchange -> write(exchange, gateArm()));
            server.createContext("/maintenance/gate/release", exchange -> write(exchange, gateRelease()));
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start maintenance endpoint " + endpoint, error);
        }
    }

    private Map<String, Object> relocate(HttpExchange exchange) {
        String modeText = query(exchange, "mode", "planned-maintenance");
        ZLinkFrameworkRelocationMode mode = switch (modeText.toLowerCase()) {
            case "planned-maintenance", "plannedmaintenance" ->
                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE;
            case "rolling-update", "rollingupdate" ->
                ZLinkFrameworkRelocationMode.ROLLING_UPDATE;
            default -> throw new IllegalArgumentException("unknown relocation mode " + modeText);
        };
        Long target = null;
        String targetText = query(exchange, "targetApplicationVersion", "");
        if (!targetText.isBlank()) {
            target = Long.valueOf(targetText);
        }
        long deadlineMs = queryLong(exchange, "deadlineMs", 30_000L);
        ZLinkFrameworkRelocationOptions options =
            new ZLinkFrameworkRelocationOptions(mode, target, Duration.ofMillis(deadlineMs));
        return relocation(runtime().relocate(options).toCompletableFuture().join());
    }

    private Map<String, Object> shutdown(HttpExchange exchange) {
        long deadlineMs = queryLong(exchange, "deadlineMs", 30_000L);
        return termination(runtime().shutdown(Duration.ofMillis(deadlineMs)).toCompletableFuture().join());
    }

    private Map<String, Object> createSpot(HttpExchange exchange) {
        String spotId = query(exchange, "spotId", "");
        String spotType = query(exchange, "spotType", "await-probe");
        if (spotId.isBlank()) {
            throw new IllegalArgumentException("spotId is required");
        }
        ZLinkSpotCreateResult result = spots.getOrCreate(spotId, spotType)
            .request(ZLinkMessage.of("maintenance-control"))
            .timeout(Duration.ofSeconds(30))
            .submit().toCompletableFuture().join();
        return Map.of(
            "state", result.state().name(),
            "spot", spot(result.spot()));
    }

    private Map<String, Object> closeSpot(HttpExchange exchange) {
        String spotId = query(exchange, "spotId", "");
        if (spotId.isBlank()) {
            throw new IllegalArgumentException("spotId is required");
        }
        Optional<SpotRef> found = spots.find(spotId).toCompletableFuture().join();
        boolean closed = found.isPresent()
            && spots.close(found.get()).toCompletableFuture().join();
        return Map.of("closed", closed, "spotId", spotId);
    }

    private Map<String, Object> status() {
        ZLinkFrameworkRuntimeStatus value = runtime().status();
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("nodeRid", config.nodeRid());
        result.put("applicationVersion", config.applicationVersion());
        result.put("placementWeight", config.placementWeight());
        result.put("automaticTopology", config.automaticTopology());
        result.put("state", value.state().name());
        result.put("isReady", value.isReady());
        result.put("acceptingWork", value.acceptingWork());
        result.put("deadline", value.deadline().map(Object::toString).orElse(""));
        result.put("sequence", value.sequence());
        result.put("relocationResult", value.relocationResult()
            .map(MaintenanceHttpServer::relocation).orElse(null));
        result.put("terminationResult", value.terminationResult()
            .map(MaintenanceHttpServer::termination).orElse(null));
        result.put("topology", topology());
        result.put("gate", gate.snapshot());
        result.put("metrics", metrics());
        return result;
    }

    private Map<String, Object> objects(HttpExchange exchange) {
        Map<String, Object> result = new LinkedHashMap<>();
        String spotId = query(exchange, "spotId", "");
        if (!spotId.isBlank()) {
            result.put("spot", spots.find(spotId).toCompletableFuture().join()
                .map(MaintenanceHttpServer::spot).orElse(null));
        }
        String actorId = query(exchange, "actorId", "");
        if (!actorId.isBlank()) {
            result.put("actor", runtime().actorManager().find(actorId).toCompletableFuture().join()
                .map(MaintenanceHttpServer::actor).orElse(null));
        }
        if (spotId.isBlank() && actorId.isBlank()) {
            throw new IllegalArgumentException("spotId or actorId is required");
        }
        return result;
    }

    private List<Map<String, Object>> topology() {
        return runtime().monitoringLocationRuntimeQuery()
            .listTopology(ZLinkLocationTopologyFilter.all(), new ZLinkPageRequest(1_000, null))
            .toCompletableFuture().join().items().stream().map(entry -> {
                Map<String, Object> row = new LinkedHashMap<>();
                row.put("meshName", entry.meshName());
                row.put("nodeRid", entry.nodeRid().toString());
                row.put("endpoint", entry.endpoint());
                row.put("draining", entry.draining());
                row.put("state", entry.state().name());
                row.put("updatedAt", entry.updatedAt() == null ? "" : entry.updatedAt().toString());
                return row;
            }).toList();
    }

    private List<Map<String, Object>> metrics() {
        List<Map<String, Object>> rows = new ArrayList<>();
        for (Meter meter : metrics.getMeters()) {
            Map<String, Object> row = new LinkedHashMap<>();
            row.put("name", meter.getId().getName());
            row.put("type", meter.getId().getType().name());
            Map<String, String> tags = new LinkedHashMap<>();
            meter.getId().getTags().forEach(tag -> tags.put(tag.getKey(), tag.getValue()));
            row.put("tags", tags);
            double value = 0;
            for (var measurement : meter.measure()) {
                value = Math.max(value, measurement.getValue());
            }
            row.put("value", value);
            rows.add(row);
        }
        return rows;
    }

    private Map<String, Object> gateArm() {
        gate.arm();
        return gate.snapshot();
    }

    private Map<String, Object> gateRelease() {
        gate.release();
        return gate.snapshot();
    }

    private static Map<String, Object> spot(SpotRef value) {
        return Map.of(
            "spotId", value.spotId(),
            "objectGeneration", value.objectGeneration(),
            "meshName", value.meshName(),
            "nodeRid", value.nodeRid().toString());
    }

    private static Map<String, Object> actor(ActorRef value) {
        return Map.of(
            "actorId", value.actorId(),
            "objectGeneration", value.objectGeneration(),
            "meshName", value.meshName(),
            "nodeRid", value.nodeRid().toString());
    }

    private ZLinkFrameworkRuntime runtime() {
        return runtimeProvider.getObject();
    }

    private static Map<String, Object> relocation(ZLinkFrameworkRelocationResult value) {
        return Map.of(
            "mode", value.mode().name(),
            "effectiveTargetApplicationVersion", value.effectiveTargetApplicationVersion(),
            "outcome", value.outcome().name(),
            "reason", value.reason().name());
    }

    private static Map<String, Object> termination(ZLinkFrameworkTerminationResult value) {
        return Map.of("outcome", value.outcome().name(), "reason", value.reason().name());
    }

    private static String query(HttpExchange exchange, String name, String fallback) {
        String raw = exchange.getRequestURI().getRawQuery();
        if (raw == null) {
            return fallback;
        }
        for (String part : raw.split("&")) {
            String[] fields = part.split("=", 2);
            if (fields.length == 2 && name.equals(fields[0])) {
                return URLDecoder.decode(fields[1], StandardCharsets.UTF_8);
            }
        }
        return fallback;
    }

    private static long queryLong(HttpExchange exchange, String name, long fallback) {
        return Long.parseLong(query(exchange, name, Long.toString(fallback)));
    }

    private void write(HttpExchange exchange, Object value) throws IOException {
        try {
            byte[] body = json.writeValueAsBytes(value);
            exchange.getResponseHeaders().set("Content-Type", "application/json");
            exchange.sendResponseHeaders(200, body.length);
            exchange.getResponseBody().write(body);
        } finally {
            exchange.close();
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
}
