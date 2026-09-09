/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executors;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Shared source-A trigger, phase, idempotency and stats owner. */
public final class BenchHttpApplication implements AutoCloseable {
    private static final Set<String> FIELDS = Set.of(
        "runId", "cellId", "pattern", "payloadBytes", "phase", "durationMs",
        "requestWindow", "sendConcurrency");
    private static final Set<String> PATTERNS = Set.of(
        "request-serial", "request-window", "request-backpressure", "send-saturation");
    private static final Pattern KEY = Pattern.compile("\"([^\"]+)\"\\s*:");

    private final HttpServer triggerServer;
    private final HttpServer statsServer;

    private BenchHttpApplication(HttpServer triggerServer, HttpServer statsServer) {
        this.triggerServer = triggerServer;
        this.statsServer = statsServer;
    }

    public static BenchHttpApplication start(
        String triggerUrl, String statsUrl, Controller controller) throws IOException {
        HttpServer trigger = create(triggerUrl);
        HttpServer stats = create(statsUrl);
        trigger.createContext("/bench/start", exchange -> {
            if (!"POST".equals(exchange.getRequestMethod())) {
                respond(exchange, 404, "{}");
                return;
            }
            try {
                byte[] bytes = exchange.getRequestBody().readNBytes(65_537);
                if (bytes.length > 65_536) {
                    throw new IllegalArgumentException("trigger body is too large");
                }
                Outcome outcome = controller.start(parse(new String(bytes, StandardCharsets.UTF_8)));
                respond(exchange, outcome.status(), outcome.body());
            } catch (IllegalArgumentException error) {
                respond(exchange, 400, "{\"reason\":" + quote(error.getMessage()) + "}");
            }
        });
        stats.createContext("/bench/stats", exchange -> {
            if (!"GET".equals(exchange.getRequestMethod())) {
                respond(exchange, 404, "{}");
                return;
            }
            respond(exchange, 200, controller.snapshotJson());
        });
        trigger.start();
        stats.start();
        return new BenchHttpApplication(trigger, stats);
    }

    private static HttpServer create(String url) throws IOException {
        URI parsed = URI.create(url);
        HttpServer server = HttpServer.create(
            new InetSocketAddress(parsed.getHost(), parsed.getPort()), 64);
        server.setExecutor(Executors.newVirtualThreadPerTaskExecutor());
        return server;
    }

    @Override
    public void close() {
        triggerServer.stop(0);
        statsServer.stop(0);
    }

    public static final class Controller {
        private final BooleanSupplier ready;
        private final Supplier<Counters> counters;
        private final Workload workload;
        private final Map<String, String> acknowledgements = new HashMap<>();
        private volatile String phase = "idle";
        private volatile String failure;
        private volatile ObservedTrigger lastTrigger;

        public Controller(
            BooleanSupplier ready, Supplier<Counters> counters, Workload workload) {
            this.ready = ready;
            this.counters = counters;
            this.workload = workload;
        }

        private synchronized Outcome start(Trigger request) {
            String key = request.runId() + "/" + request.cellId() + "/" + request.phase();
            String previous = acknowledgements.get(key);
            if (previous != null) {
                return new Outcome(200, previous);
            }
            if (!ready.getAsBoolean()) {
                return reject(request, "source is not ready");
            }
            if (!"idle".equals(phase)) {
                return reject(request, "phase " + phase + " has not completed");
            }
            long receivedAt = System.currentTimeMillis();
            long startedAt = BenchMetricHeader.nowNs();
            lastTrigger = new ObservedTrigger(request, receivedAt);
            phase = request.phase();
            failure = null;
            String reply = "{\"accepted\":true,\"runId\":" + quote(request.runId())
                + ",\"cellId\":" + quote(request.cellId()) + ",\"phase\":"
                + quote(request.phase()) + ",\"startedAt\":" + startedAt + "}";
            acknowledgements.put(key, reply);
            Thread.ofPlatform().daemon(true).name("bench-phase-" + request.phase()).start(() -> {
                try {
                    workload.run(request);
                    phase = "idle";
                } catch (Throwable error) {
                    failure = describe(error);
                    phase = "failed";
                    error.printStackTrace(System.err);
                }
            });
            return new Outcome(200, reply);
        }

        private Outcome reject(Trigger request, String reason) {
            String reply = "{\"accepted\":false,\"runId\":" + quote(request.runId())
                + ",\"cellId\":" + quote(request.cellId()) + ",\"phase\":"
                + quote(request.phase()) + ",\"startedAt\":" + BenchMetricHeader.nowNs()
                + ",\"reason\":" + quote(reason) + "}";
            return new Outcome(409, reply);
        }

        public ObservedTrigger lastTrigger() {
            return lastTrigger;
        }

        public String snapshotJson() {
            Counters value = counters.get();
            return "{\"ready\":" + ready.getAsBoolean()
                + ",\"phase\":" + quote(phase)
                + ",\"submitted\":" + value.submitted()
                + ",\"completed\":" + value.completed()
                + ",\"errors\":" + value.errors()
                + ",\"received\":0"
                + ",\"inFlight\":" + value.inFlight()
                + ",\"currentInFlight\":" + value.inFlight()
                + ",\"peakInFlight\":" + value.peakInFlight()
                + ",\"failure\":" + (failure == null ? "null" : quote(failure)) + "}";
        }
    }

    private static Trigger parse(String json) {
        Set<String> keys = new HashSet<>();
        Matcher matcher = KEY.matcher(json);
        while (matcher.find()) {
            keys.add(matcher.group(1));
        }
        if (!keys.equals(FIELDS)) {
            Set<String> missing = new HashSet<>(FIELDS);
            missing.removeAll(keys);
            Set<String> unknown = new HashSet<>(keys);
            unknown.removeAll(FIELDS);
            throw new IllegalArgumentException(
                "trigger fields mismatch; missing=" + missing + " unknown=" + unknown);
        }
        Trigger value = new Trigger(
            stringField(json, "runId"), stringField(json, "cellId"),
            stringField(json, "pattern"), intField(json, "payloadBytes"),
            stringField(json, "phase"), intField(json, "durationMs"),
            intField(json, "requestWindow"), intField(json, "sendConcurrency"));
        if (value.runId().isBlank() || value.cellId().isBlank()) {
            throw new IllegalArgumentException("runId and cellId must be non-empty");
        }
        if (!PATTERNS.contains(value.pattern())) {
            throw new IllegalArgumentException("unknown benchmark pattern");
        }
        if (!Set.of("warmup", "active").contains(value.phase())) {
            throw new IllegalArgumentException("phase must be warmup or active");
        }
        if (value.payloadBytes() < BenchMetricHeader.HEADER_SIZE || value.durationMs() <= 0
            || value.requestWindow() <= 0 || value.sendConcurrency() <= 0) {
            throw new IllegalArgumentException("numeric trigger fields are invalid");
        }
        return value;
    }

    private static String stringField(String json, String name) {
        Matcher matcher = Pattern.compile(
            "\"" + Pattern.quote(name) + "\"\\s*:\\s*\"((?:\\\\.|[^\"])*)\"")
            .matcher(json);
        if (!matcher.find()) {
            throw new IllegalArgumentException(name + " must be a string");
        }
        return matcher.group(1).replace("\\\"", "\"").replace("\\\\", "\\");
    }

    private static int intField(String json, String name) {
        Matcher matcher = Pattern.compile(
            "\"" + Pattern.quote(name) + "\"\\s*:\\s*(-?[0-9]+)").matcher(json);
        if (!matcher.find()) {
            throw new IllegalArgumentException(name + " must be an integer");
        }
        return Integer.parseInt(matcher.group(1));
    }

    private static void respond(HttpExchange exchange, int status, String body)
        throws IOException {
        byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().set("content-type", "application/json");
        exchange.sendResponseHeaders(status, bytes.length);
        try (var output = exchange.getResponseBody()) {
            output.write(bytes);
        }
    }

    public static String quote(String text) {
        if (text == null) {
            return "null";
        }
        return "\"" + text.replace("\\", "\\\\").replace("\"", "\\\"")
            .replace("\n", "\\n").replace("\r", "\\r") + "\"";
    }

    private static String describe(Throwable error) {
        StringBuilder result = new StringBuilder();
        for (Throwable current = error; current != null; current = current.getCause()) {
            if (!result.isEmpty()) {
                result.append(" <- ");
            }
            result.append(current);
            if (current.getCause() == current) {
                break;
            }
        }
        return result.toString();
    }

    public record Trigger(
        String runId, String cellId, String pattern, int payloadBytes, String phase,
        int durationMs, int requestWindow, int sendConcurrency) {
    }

    public record ObservedTrigger(Trigger trigger, long receivedAtUnixMs) {
    }

    public record Counters(
        long submitted, long completed, long errors, long inFlight, long peakInFlight) {
        public static Counters empty() {
            return new Counters(0, 0, 0, 0, 0);
        }
    }

    @FunctionalInterface
    public interface Workload {
        void run(Trigger request) throws Exception;
    }

    private record Outcome(int status, String body) {
    }
}
