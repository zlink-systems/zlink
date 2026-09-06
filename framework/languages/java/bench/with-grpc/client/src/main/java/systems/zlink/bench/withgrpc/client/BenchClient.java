/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.ZonedDateTime;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import systems.zlink.bench.withgrpc.shared.BenchContract;
import systems.zlink.bench.withgrpc.shared.RawWire;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;

/**
 * with-grpc local bench, java client. One client process and one server process per
 * implementation (spec section 3). Emits {@code cells.json} in the
 * {@code with-grpc-cell-v1} shape plus RESULT lines; every table, ratio and verdict is
 * produced by framework/bench/tools, not here (plan section 4.1, FB-020).
 */
public final class BenchClient {
    private static final List<String> PATTERNS =
        List.of("request-serial", "request-window", "send-saturation");

    private final BenchOptions options;
    private final StatsClient stats = new StatsClient();
    private final BenchDrivers drivers;
    private final List<Map<String, Object>> cells = new ArrayList<>();
    private final List<String> failures = new ArrayList<>();
    private final List<String> contaminated = new ArrayList<>();
    private final List<String> drainObservations = new ArrayList<>();
    private final Map<String, String> pendingContamination = new HashMap<>();

    private BenchClient(BenchOptions options) {
        this.options = options;
        this.drivers = new BenchDrivers(options, stats);
    }

    public static void main(String[] args) throws Exception {
        BenchOptions options = new BenchOptions(args);
        BenchClient client = new BenchClient(options);
        int status = client.run();
        System.exit(status);
    }

    private int run() throws Exception {
        Files.createDirectories(Path.of(options.output));

        Context context = Zlink.createContext();
        GrpcStack grpc = options.runsImplementation("grpc-java")
            ? new GrpcStack(options) : null;

        // The framework host is built once, outside every measured window. A failure to
        // stand it up marks the six framework cells unsupported with the reason rather
        // than taking the run down.
        FrameworkStack framework = null;
        String frameworkError = null;
        if (options.runsImplementation("zlink-framework-java")) {
            try {
                framework = FrameworkStack.create(options);
            } catch (Exception error) {
                frameworkError = String.valueOf(error);
                System.err.println("[bench] framework client unavailable: " + frameworkError);
            }
        }

        for (int payloadSize : options.payloadSizes) {
            for (String pattern : PATTERNS) {
                if (!options.runsPattern(pattern)) {
                    continue;
                }
                if (grpc != null) {
                    runGrpc(grpc, pattern, payloadSize);
                }
                if (options.runsImplementation("zlink-java")) {
                    runRaw(context, pattern, payloadSize);
                }
                if (options.runsImplementation("zlink-framework-java")) {
                    runFramework(framework, frameworkError, pattern, payloadSize);
                }
            }
        }

        writeOutputs();
        System.err.println("[bench] cells completed=" + cells.size()
            + " failed=" + failures.size() + " contaminated=" + contaminated.size());
        if (grpc != null) {
            grpc.close();
        }
        if (framework != null) {
            framework.close();
        }
        context.close();
        return 0;
    }

    private void runGrpc(GrpcStack grpc, String pattern, int payloadSize) {
        addCell("grpc-java", pattern, payloadSize, () -> switch (pattern) {
            case "request-serial" -> drivers.runRequestSerial(
                payloadSize, options.grpcStatsUrl, grpc.echo());
            case "request-window" -> drivers.runRequestWindow(
                payloadSize, options.grpcStatsUrl, grpc.echo());
            default -> drivers.runSendSaturation(
                payloadSize, options.grpcStatsUrl, grpc.command());
        });
    }

    private void runRaw(Context context, String pattern, int payloadSize) {
        addCell("zlink-java", pattern, payloadSize, () -> {
            boolean isSend = "send-saturation".equals(pattern);
            String peer = isSend
                ? RawWire.RAW_COMMAND_SERVER_ID : RawWire.RAW_REQUEST_SERVER_ID;
            String endpoint = isSend
                ? options.zlinkRawCommandEndpoint : options.zlinkRawEndpoint;
            String selfId = "bench-" + pattern.charAt(8) + "-"
                + ProcessHandle.current().pid() + "-s" + payloadSize;
            try (RawStack raw = RawStack.create(
                context, options, selfId, peer, endpoint)) {
                BenchOperation operation = isSend ? raw.send() : raw.request();
                drivers.waitForRouteReady(operation, payloadSize);
                return switch (pattern) {
                    case "request-serial" -> drivers.runRequestSerial(
                        payloadSize, options.zlinkRawStatsUrl, operation);
                    case "request-window" -> drivers.runRequestWindow(
                        payloadSize, options.zlinkRawStatsUrl, operation);
                    default -> drivers.runSendSaturation(
                        payloadSize, options.zlinkRawStatsUrl, operation);
                };
            }
        });
    }

    private void runFramework(
        FrameworkStack framework, String frameworkError, String pattern, int payloadSize) {
        addCell("zlink-framework-java", pattern, payloadSize, () -> {
            if (framework == null) {
                throw new IllegalStateException(
                    "framework host unavailable via zlink-framework-spring-boot-starter: "
                        + frameworkError);
            }
            BenchOperation operation = "send-saturation".equals(pattern)
                ? framework.send() : framework.request();
            drivers.waitForRouteReady(operation, payloadSize);
            return switch (pattern) {
                case "request-serial" -> drivers.runRequestSerial(
                    payloadSize, options.zlinkStatsUrl, operation);
                case "request-window" -> drivers.runRequestWindow(
                    payloadSize, options.zlinkStatsUrl, operation);
                default -> drivers.runSendSaturation(
                    payloadSize, options.zlinkStatsUrl, operation);
            };
        });
    }

    /**
     * Cell isolation: one cell that throws must not take the other 17 with it. No retry
     * and no fabricated value -- a failed cell is simply absent, which the aggregator
     * reads as {@code unsupported}.
     */
    private void addCell(
        String implementation, String pattern, int payloadSize, CellBody body) {
        String name = implementation + "-" + pattern;
        String reason = pendingContamination.remove(implementation);
        if (reason != null) {
            contaminated.add(name + "@" + payloadSize + ": " + reason);
            System.err.println(
                "[bench] CONTAMINATED " + name + "@" + payloadSize + ": " + reason);
            return;
        }
        System.err.println("[bench] running " + name + "@" + payloadSize);
        try {
            Map<String, Object> measured = body.run();
            Map<String, Object> cell = new LinkedHashMap<>();
            cell.put("implementation", implementation);
            cell.put("pattern", pattern);
            cell.put("payload_size", payloadSize);
            cell.put("contaminated", false);
            cell.put("contamination_reason", null);
            cell.putAll(measured);
            cells.add(cell);
            if (measured.containsKey("drain_ms")) {
                double drainMs = ((Number) measured.get("drain_ms")).doubleValue();
                boolean boundHit = Boolean.TRUE.equals(measured.get("drain_bound_hit"));
                drainObservations.add(String.format(
                    "%s@%d: drain_ms=%.0f bound_hit=%s bound_ms=%d",
                    name, payloadSize, drainMs, boundHit, options.drainBoundMs));
                if (boundHit) {
                    // FB-008: the next cell on this same server stands behind the
                    // backlog this one left. Mark it and exclude it rather than
                    // measuring it.
                    pendingContamination.put(implementation, String.format(
                        "previous cell %s@%d did not drain within %dms (observed %.0fms)",
                        name, payloadSize, options.drainBoundMs, drainMs));
                }
            }
            System.err.printf(
                "[bench] finished %s@%d throughput=%.1f/s cores=%.2f peak_in_flight=%s%n",
                name, payloadSize,
                ((Number) measured.get("throughput_per_second")).doubleValue(),
                ((Number) measured.get("client_cores")).doubleValue(),
                measured.get("peak_in_flight"));
        } catch (Throwable error) {
            // The cause chain is kept: a bare toString() hides which layer refused,
            // and a failed cell has to be explainable from the run artefacts alone.
            failures.add(name + "@" + payloadSize + ": " + describe(error));
            System.err.println(
                "[bench] FAILED " + name + "@" + payloadSize + ": " + describe(error));
            error.printStackTrace();
        }
    }

    private static String describe(Throwable error) {
        StringBuilder out = new StringBuilder();
        Throwable current = error;
        while (current != null) {
            if (out.length() > 0) {
                out.append(" <- ");
            }
            out.append(current);
            current = current.getCause() == current ? null : current.getCause();
        }
        return out.toString();
    }

    @FunctionalInterface
    private interface CellBody {
        Map<String, Object> run() throws Exception;
    }

    // --- outputs -----------------------------------------------------------

    private void writeOutputs() throws IOException {
        Map<String, Object> metadata = collectMetadata();
        StringBuilder json = new StringBuilder();
        json.append("{\n  \"schema\": \"with-grpc-cell-v1\",\n  \"metadata\": ");
        Json.write(json, metadata, 1);
        json.append(",\n  \"cells\": [\n");
        for (int i = 0; i < cells.size(); i++) {
            json.append("    ");
            Json.write(json, cells.get(i), 2);
            json.append(i + 1 < cells.size() ? ",\n" : "\n");
        }
        json.append("  ]\n}\n");
        Files.writeString(Path.of(options.output, "cells.json"), json.toString(),
            StandardCharsets.UTF_8);

        List<String> lines = new ArrayList<>();
        lines.add("# with-grpc bench, java row");
        for (Map.Entry<String, Object> entry : metadata.entrySet()) {
            if (entry.getValue() instanceof Map || entry.getValue() instanceof List) {
                continue;
            }
            lines.add("  " + entry.getKey() + ": " + entry.getValue());
        }
        lines.add("");
        for (Map<String, Object> cell : cells) {
            String scenario = cell.get("implementation") + "-" + cell.get("pattern");
            for (Map.Entry<String, String> metric : resultMetrics().entrySet()) {
                Object value = cell.get(metric.getValue());
                if (value == null) {
                    continue;
                }
                lines.add(String.format("RESULT,current,%s,local,%s,%s,%.3f",
                    scenario, cell.get("payload_size"), metric.getKey(),
                    ((Number) value).doubleValue()));
            }
        }
        String report = String.join("\n", lines) + "\n";
        Files.writeString(Path.of(options.output, options.reportFile), report,
            StandardCharsets.UTF_8);
        Files.writeString(Path.of(options.output, "report.txt"), report,
            StandardCharsets.UTF_8);

        if (!failures.isEmpty() || !contaminated.isEmpty() || !drainObservations.isEmpty()) {
            List<String> summary = new ArrayList<>();
            if (!drainObservations.isEmpty()) {
                summary.add("## Drain (FB-008)");
                drainObservations.forEach(line -> summary.add("- " + line));
                summary.add("");
            }
            if (!contaminated.isEmpty()) {
                summary.add("## Contaminated (excluded from tables and judgement)");
                contaminated.forEach(line -> summary.add("- " + line));
                summary.add("");
            }
            summary.add("## Failures");
            failures.forEach(line -> summary.add("- " + line));
            Files.writeString(Path.of(options.output, "failures.txt"),
                String.join("\n", summary) + "\n", StandardCharsets.UTF_8);
            System.err.println(String.join("\n", summary));
        }
    }

    private static Map<String, String> resultMetrics() {
        Map<String, String> metrics = new LinkedHashMap<>();
        metrics.put("throughput", "throughput_per_second");
        metrics.put("bandwidth", "bandwidth_mb_s");
        metrics.put("latency", "latency_mean_ms");
        metrics.put("latency_p95", "latency_p95_ms");
        metrics.put("latency_p99", "latency_p99_ms");
        metrics.put("client_cpu_percent", "client_cpu_percent");
        metrics.put("client_memory_mb", "client_memory_mb");
        metrics.put("server_cpu_percent", "server_cpu_percent");
        metrics.put("server_memory_mb", "server_memory_mb");
        return metrics;
    }

    private Map<String, Object> collectMetadata() {
        Map<String, Object> metadata = new LinkedHashMap<>();
        metadata.put("diagnosticsSchema", "with-grpc-cell-v1");
        metadata.put("language", "java");
        metadata.put("javaVersion", System.getProperty("java.version"));
        metadata.put("javaVendor", System.getProperty("java.vendor"));
        metadata.put("javaVmName", System.getProperty("java.vm.name"));
        metadata.put("grpcJavaVersion", "1.72.0");
        metadata.put("protobufJavaVersion", "4.30.2");
        metadata.put("grpcServerConfiguration",
            stats.info(options.grpcStatsUrl).replace("\"", "'"));
        metadata.put("zlinkBindingVersion", "0.17.0");
        metadata.put("frameworkHost", "zlink-framework-spring-boot-starter 0.10.0");
        metadata.put("logical_cores", ClientResources.LOGICAL_CORES);
        metadata.put("client_saturation_metric", ClientResources.CLIENT_SATURATION_METRIC);
        // Declared per driver, not per run: 1 for the serial and window drivers, the
        // send concurrency for the send driver. The per-cell value is authoritative.
        metadata.put("client_parallelism_ceiling", "1 (request drivers) / "
            + options.sendConcurrency + " (send driver)");
        metadata.put("cpu", readCpuModel());
        metadata.put("kernel", System.getProperty("os.version"));
        metadata.put("commit", readCommit());
        metadata.put("rawSocket", options.rawSocket);
        metadata.put("warmupSeconds", options.warmupSeconds);
        metadata.put("warmupSegmentSeconds", options.warmupSegmentSeconds);
        metadata.put("durationSeconds", options.durationSeconds);
        metadata.put("requestWindow", options.requestWindow);
        metadata.put("sendConcurrency", options.sendConcurrency);
        metadata.put("requestTimeoutMs", options.requestTimeoutMs);
        metadata.put("channel", BenchContract.CHANNEL_NAME);
        metadata.put("runId", options.runId);
        metadata.put("startedAt", ZonedDateTime.now().toString());
        metadata.put("loadavg1", readLoadavg());
        metadata.put("contaminatedCells", contaminated);
        return metadata;
    }

    private static String readCpuModel() {
        try {
            for (String line : Files.readAllLines(Path.of("/proc/cpuinfo"))) {
                if (line.startsWith("model name")) {
                    return line.substring(line.indexOf(':') + 1).trim();
                }
            }
        } catch (IOException error) {
            // provenance is best effort; the run still reports
        }
        return "unknown";
    }

    private static String readCommit() {
        try {
            Process process = new ProcessBuilder("git", "rev-parse", "HEAD")
                .redirectErrorStream(true).start();
            String output = new String(process.getInputStream().readAllBytes(),
                StandardCharsets.UTF_8).trim();
            process.waitFor();
            return output.isEmpty() ? "unknown" : output;
        } catch (Exception error) {
            return "unknown";
        }
    }

    private static double readLoadavg() {
        try {
            return Double.parseDouble(
                Files.readString(Path.of("/proc/loadavg")).trim().split("\\s+")[0]);
        } catch (Exception error) {
            return -1;
        }
    }
}
