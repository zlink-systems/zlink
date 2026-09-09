/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.LinkedHashMap;
import java.util.Map;
import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;

/** Writes one source-A cell in the aggregator's with-grpc-cell-v1 form. */
public final class BenchResultWriter {
    private BenchResultWriter() {
    }

    public static void write(
        BenchOptions options,
        BenchHttpApplication.ObservedTrigger observed,
        Map<String, Object> result,
        String language,
        String streamImplementation,
        Map<String, Object> metadataAdditions) throws IOException {
        Map<String, Object> trigger = new LinkedHashMap<>();
        trigger.put("runId", observed.trigger().runId());
        trigger.put("cellId", observed.trigger().cellId());
        trigger.put("pattern", observed.trigger().pattern());
        trigger.put("payloadBytes", observed.trigger().payloadBytes());
        trigger.put("durationMs", observed.trigger().durationMs());
        trigger.put("warmup", options.warmupSeconds);
        trigger.put("endpoint", options.triggerUrl + "/bench/start");
        trigger.put("receivedAtUnixMs", observed.receivedAtUnixMs());

        Map<String, Object> streams = new LinkedHashMap<>();
        switch (options.scenario) {
            case "request-serial" -> {
                streams.put("count", 1);
                streams.put("inFlightPerStream", 1);
            }
            case "request-window" -> {
                streams.put("count", 1);
                streams.put("inFlightPerStream", options.requestWindow);
            }
            case "request-backpressure" -> {
                streams.put("count", 1);
                streams.put("inFlightPerStream", null);
            }
            case "send-saturation" -> {
                streams.put("count", options.sendConcurrency);
                streams.put("inFlightPerStream", 1);
            }
            default -> throw new IllegalArgumentException("unknown pattern " + options.scenario);
        }
        streams.put("implementation", streamImplementation);

        Map<String, Object> cell = new LinkedHashMap<>();
        cell.put("implementation", options.implementation);
        cell.put("pattern", options.scenario);
        cell.put("payload_size", options.payloadSizes.get(0));
        cell.put("role", "source");
        cell.put("trigger", trigger);
        cell.put("streams", streams);
        cell.put("target_stats", null);
        cell.putAll(result);

        Map<String, Object> metadata = new LinkedHashMap<>();
        metadata.put("diagnosticsSchema", "with-grpc-cell-v1");
        metadata.put("language", language);
        metadata.put("javaVersion", System.getProperty("java.version"));
        metadata.put("javaVendor", System.getProperty("java.vendor"));
        metadata.put("javaVmName", System.getProperty("java.vm.name"));
        metadata.put("grpcJavaVersion", "1.72.0");
        metadata.put("protobufJavaVersion", "4.30.2");
        metadata.put("zlinkBindingVersion", "0.17.6");
        metadata.put("frameworkVersion", "0.10.0");
        metadata.put("logicalCores", ClientResources.LOGICAL_CORES);
        metadata.put("clientSaturationMetric", ClientResources.CLIENT_SATURATION_METRIC);
        metadata.put("requestWindow", options.requestWindow);
        metadata.put("sendConcurrency", options.sendConcurrency);
        metadata.put("warmupSeconds", options.warmupSeconds);
        metadata.put("triggerUrl", options.triggerUrl);
        metadata.put("statsUrl", options.statsUrl);
        metadata.put("targetEndpoint", options.targetEndpoint);
        metadata.put("targetCommandEndpoint",
            options.targetCommandEndpoint.isBlank() ? null : options.targetCommandEndpoint);
        metadata.put("targetStatsUrl", options.targetStatsUrl);
        metadata.put("rawSocket", options.rawSocket);
        metadata.put("commit", readCommit());
        metadata.put("generatedUtc", Instant.now().toString());
        metadata.putAll(metadataAdditions);

        Files.createDirectories(Path.of(options.output));
        StringBuilder json = new StringBuilder();
        json.append("{\n  \"schema\": \"with-grpc-cell-v1\",\n  \"metadata\": ");
        Json.write(json, metadata, 1);
        json.append(",\n  \"cells\": [\n    ");
        Json.write(json, cell, 2);
        json.append("\n  ]\n}\n");
        Files.writeString(Path.of(options.output, "results.json"), json.toString(),
            StandardCharsets.UTF_8);

        Map<String, String> fields = new LinkedHashMap<>();
        fields.put("throughput", "throughput_per_second");
        fields.put("bandwidth", "bandwidth_mb_s");
        fields.put("latency", "latency_mean_ms");
        fields.put("latency_p95", "latency_p95_ms");
        fields.put("latency_p99", "latency_p99_ms");
        fields.put("client_cpu_percent", "client_cpu_percent");
        fields.put("client_memory_mb", "client_memory_mb");
        fields.put("server_cpu_percent", "server_cpu_percent");
        fields.put("server_memory_mb", "server_memory_mb");
        String scenario = options.implementation + "-" + options.scenario;
        StringBuilder report = new StringBuilder("# with-grpc bench, server-driven source A\n\n");
        for (Map.Entry<String, String> entry : fields.entrySet()) {
            Number value = (Number) result.get(entry.getValue());
            report.append(String.format("RESULT,current,%s,local,%d,%s,%.3f%n",
                scenario, options.payloadSizes.get(0), entry.getKey(), value.doubleValue()));
        }
        Files.writeString(Path.of(options.output, options.reportFile), report.toString(),
            StandardCharsets.UTF_8);
        System.out.print(report);
        System.out.flush();
    }

    private static String readCommit() {
        try {
            Process process = new ProcessBuilder("git", "rev-parse", "HEAD")
                .redirectErrorStream(true).start();
            String output = new String(process.getInputStream().readAllBytes(),
                StandardCharsets.UTF_8).trim();
            return process.waitFor() == 0 ? output : "unknown";
        } catch (Exception error) {
            return "unknown";
        }
    }
}
