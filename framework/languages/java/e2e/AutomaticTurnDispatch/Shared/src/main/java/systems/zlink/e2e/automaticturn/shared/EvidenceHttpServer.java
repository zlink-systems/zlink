package systems.zlink.e2e.automaticturn.shared;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import org.springframework.context.SmartLifecycle;
import io.micrometer.core.instrument.Meter;
import io.micrometer.core.instrument.MeterRegistry;
import java.util.ArrayList;
import java.util.List;
import java.util.LinkedHashMap;
import java.util.Map;
import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery;
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult;

public final class EvidenceHttpServer implements SmartLifecycle {
    private final EvidenceStore evidence;
    private final ObjectMapper json;
    private final String endpoint;
    private final MeterRegistry metrics;
    private final ZLinkFrameworkLifecycle drain;
    private final java.util.function.Supplier<ZLinkLocationRuntimeQuery> locations;
    private final DrainEvidence drainEvidence;
    private final java.util.function.Function<systems.zlink.contracts.core.RoutingId,
        CompletionStage<Boolean>> closeSpot;
    private final java.util.function.Supplier<CompletionStage<String>> routeProbe;
    private volatile CompletionStage<ZLinkFrameworkTerminationResult> drainResult;
    private HttpServer server;
    private boolean running;

    public EvidenceHttpServer(EvidenceStore evidence, ObjectMapper json, String endpoint) {
        this(evidence, json, endpoint, null);
    }

    public EvidenceHttpServer(
        EvidenceStore evidence,
        ObjectMapper json,
        String endpoint,
        MeterRegistry metrics) {
        this(evidence, json, endpoint, metrics, null, null, null, null, null);
    }

    public EvidenceHttpServer(
        EvidenceStore evidence,
        ObjectMapper json,
        String endpoint,
        MeterRegistry metrics,
        ZLinkFrameworkLifecycle drain,
        java.util.function.Supplier<ZLinkLocationRuntimeQuery> locations,
        DrainEvidence drainEvidence,
        java.util.function.Function<systems.zlink.contracts.core.RoutingId,
            CompletionStage<Boolean>> closeSpot,
        java.util.function.Supplier<CompletionStage<String>> routeProbe) {
        this.evidence = evidence;
        this.json = json;
        this.endpoint = endpoint;
        this.metrics = metrics;
        this.drain = drain;
        this.locations = locations;
        this.drainEvidence = drainEvidence;
        this.closeSpot = closeSpot;
        this.routeProbe = routeProbe;
    }

    @Override
    public void start() {
        if (endpoint == null || endpoint.isBlank()) {
            return;
        }
        try {
            URI uri = URI.create(endpoint);
            server = HttpServer.create(new InetSocketAddress(uri.getHost(), uri.getPort()), 0);
            server.createContext("/health", exchange -> write(exchange, "ok\n"));
            server.createContext("/evidence", exchange -> write(
                exchange,
                json.writeValueAsString(evidence.snapshot())));
            server.createContext("/metrics", exchange -> write(
                exchange,
                json.writeValueAsString(metricSnapshot())));
            server.createContext("/metrics-ready", exchange -> write(
                exchange,
                Boolean.toString(systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.enabled())));
            server.createContext("/drain/start", exchange -> {
                if (drain == null) {
                    write(exchange, "{\"accepted\":false}");
                    return;
                }
                long deadlineMs = queryLong(exchange.getRequestURI().getRawQuery(), "deadlineMs", 30000L);
                drainResult = drain.shutdown(Duration.ofMillis(deadlineMs));
                write(exchange, "{\"accepted\":true}");
            });
            server.createContext("/drain/status", exchange -> write(
                exchange, json.writeValueAsString(drainSnapshot())));
            server.createContext("/spot/close", exchange -> {
                if (closeSpot == null) {
                    write(exchange, "{\"closed\":false}");
                    return;
                }
                String spotRid = queryValue(exchange.getRequestURI().getRawQuery(), "spotRid");
                boolean closed = closeSpot.apply(
                    systems.zlink.contracts.core.RoutingId.from(spotRid))
                    .toCompletableFuture().join();
                write(exchange, "{\"closed\":" + closed + "}");
            });
            server.createContext("/route-probe", exchange -> {
                if (routeProbe == null) {
                    write(exchange, "{\"ready\":false}");
                    return;
                }
                String target = routeProbe.get().toCompletableFuture().join();
                write(exchange, "{\"ready\":true,\"target\":\"" + target + "\"}");
            });
            server.start();
            running = true;
        } catch (Exception error) {
            throw new IllegalStateException("failed to start evidence endpoint " + endpoint, error);
        }
    }

    private Map<String, Object> drainSnapshot() {
        Map<String, Object> snapshot = new LinkedHashMap<>();
        boolean ready = drain == null || drain.isReady();
        if (ready && drainEvidence != null) drainEvidence.observeServing();
        snapshot.put("ready", ready);
        snapshot.put("events", drainEvidence == null ? List.of() : drainEvidence.events().stream()
            .map(event -> Map.of(
                "state", event.state(),
                "timestamp", event.timestamp().toString()))
            .toList());
        if (locations != null) {
            try {
                ZLinkLocationRuntimeQuery query = locations.get();
                var status = query.getStatus().toCompletableFuture().join();
                snapshot.put("locationStatus", Map.of(
                    "storeHealthy", status.storeHealthy(),
                    "ownerLeaseHealthy", status.ownerLeaseHealthy(),
                    "ownerLeaseRenewedAt", status.ownerLeaseRenewedAt() == null
                        ? "" : status.ownerLeaseRenewedAt().toString()));
                snapshot.put("peerRows", query.listTopology(
                        ZLinkLocationTopologyFilter.all(),
                        new ZLinkPageRequest(1_000, null))
                    .toCompletableFuture().join().items().stream().map(peer -> Map.of(
                        "nodeRid", peer.nodeRid().toString(),
                        "meshName", peer.meshName(),
                        "role", "mesh-node",
                        "draining", peer.draining(),
                        "ownerId", "")).toList());
            } catch (RuntimeException error) {
                snapshot.put("locationError", error.toString());
            }
        }
        CompletionStage<ZLinkFrameworkTerminationResult> current = drainResult;
        if (current != null && current.toCompletableFuture().isDone()) {
            try {
                ZLinkFrameworkTerminationResult result = current.toCompletableFuture().join();
                snapshot.put("result", result.outcome()
                    == systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome.STOPPED
                        ? "drained" : "force_stopped");
                if (result.outcome()
                    == systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome.FORCE_STOPPED) {
                    snapshot.put("reason", result.reason().name().toLowerCase());
                }
            } catch (RuntimeException error) {
                snapshot.put("resultError", error.toString());
            }
        }
        return snapshot;
    }

    private static long queryLong(String query, String name, long fallback) {
        if (query == null) return fallback;
        for (String part : query.split("&")) {
            String[] fields = part.split("=", 2);
            if (fields.length == 2 && name.equals(fields[0])) return Long.parseLong(fields[1]);
        }
        return fallback;
    }

    private static String queryValue(String query, String name) {
        if (query == null) return "";
        for (String part : query.split("&")) {
            String[] fields = part.split("=", 2);
            if (fields.length == 2 && name.equals(fields[0])) return fields[1];
        }
        return "";
    }

    private java.util.List<Map<String, Object>> metricSnapshot() {
        if (metrics == null) {
            return java.util.List.of();
        }
        java.util.List<Map<String, Object>> rows = new ArrayList<>();
        for (Meter meter : metrics.getMeters()) {
            Map<String, Object> row = new LinkedHashMap<>();
            String name = meter.getId().getName();
            row.put("name", name);
            row.put("kind", kind(meter.getId().getType()));
            row.put("unit", unit(name));
            Map<String, String> tags = new LinkedHashMap<>();
            meter.getId().getTags().forEach(tag -> tags.put(tag.getKey(), tag.getValue()));
            row.put("tags", tags);
            double value = 0;
            long count = 0;
            for (var measurement : meter.measure()) {
                if (measurement.getStatistic() == io.micrometer.core.instrument.Statistic.COUNT) {
                    count = Math.round(measurement.getValue());
                }
                value = Math.max(value, measurement.getValue());
            }
            row.put("value", value);
            row.put("count", count);
            rows.add(row);
        }
        return rows;
    }

    private static String kind(Meter.Type type) {
        return switch (type) {
            case COUNTER -> "counter";
            case GAUGE -> "updown";
            case TIMER, DISTRIBUTION_SUMMARY, LONG_TASK_TIMER -> "histogram";
            default -> type.name().toLowerCase();
        };
    }

    private static String unit(String name) {
        if (name.endsWith("duration") || name.endsWith("lateness")) return "s";
        if (name.contains("pending_requests.count")) return "{request}";
        if (name.contains("transfers")) return "{transfer}";
        if (name.contains("actors.handed_off")) return "{actor}";
        if (name.contains("rooms.drained")) return "{room}";
        if (name.contains("fanout")) return "{message}";
        if (name.contains("reconnects")) return "{event}";
        return "{connection}";
    }

    private static void write(
        com.sun.net.httpserver.HttpExchange exchange,
        String value) throws java.io.IOException {
        byte[] body = value.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().add("Content-Type", "application/json");
        exchange.sendResponseHeaders(200, body.length);
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
}
