/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.bench.withgrpc.client;

import java.io.IOException;
import java.net.ServerSocket;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;

/** A failed active phase must not leave warmup values at the aggregate input paths. */
public final class WarmupResultIsolationTest {
    public static void main(String[] args) throws Exception {
        Path output = Files.createTempDirectory("bench-warmup-result-isolation-");
        int triggerPort = freePort();
        int statsPort = freePort();
        String triggerUrl = "http://127.0.0.1:" + triggerPort;
        String statsUrl = "http://127.0.0.1:" + statsPort;
        BenchOptions options = new BenchOptions(new String[] {
            "--scenario", "request-serial", "--implementation", "grpc-java",
            "--run-id", "warmup-result-isolation", "--trigger-url", triggerUrl,
            "--stats-url", statsUrl, "--target-stats-url", "http://127.0.0.1:1",
            "--target-endpoint", "127.0.0.1:2", "--output", output.toString()
        });
        BenchHttpApplication.Controller[] holder = new BenchHttpApplication.Controller[1];
        holder[0] = new BenchHttpApplication.Controller(
            () -> true,
            BenchHttpApplication.Counters::empty,
            trigger -> {
                if ("active".equals(trigger.phase())) {
                    throw new IOException("expected active phase failure");
                }
                BenchResultWriter.write(options, holder[0].lastTrigger(), result(), "java",
                    "warmup result isolation test", Map.of());
                return 1L;
            });

        try (BenchHttpApplication http = BenchHttpApplication.start(triggerUrl, statsUrl, holder[0])) {
            HttpClient client = HttpClient.newHttpClient();
            HttpResponse<String> warmup = trigger(client, triggerUrl, "warmup");
            check(warmup.statusCode() == 200, "warmup phase failed: " + warmup.body());
            HttpResponse<String> active = trigger(client, triggerUrl, "active");
            check(active.statusCode() == 500, "active phase did not fail: " + active.body());
            check(!Files.exists(output.resolve("results.json")),
                "warmup output occupied active aggregate path results.json");
            check(!Files.exists(output.resolve("report.txt")),
                "warmup output occupied active report path report.txt");
            check(Files.exists(output.resolve("results.warmup.json")),
                "warmup result file was not written");
            check(Files.exists(output.resolve("report.warmup.txt")),
                "warmup report file was not written");
        } finally {
            deleteTree(output);
        }
        System.out.println("WARMUP_RESULT_ISOLATION_OK");
    }

    private static HttpResponse<String> trigger(HttpClient client, String endpoint, String phase)
            throws Exception {
        String body = "{\"runId\":\"warmup-result-isolation\","
            + "\"cellId\":\"warmup-result-isolation\",\"pattern\":\"request-serial\","
            + "\"payloadBytes\":1024,\"phase\":\"" + phase + "\",\"durationMs\":1,"
            + "\"requestWindow\":100,\"sendConcurrency\":8}";
        return client.send(HttpRequest.newBuilder(URI.create(endpoint + "/bench/start"))
                .header("content-type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(body)).build(),
            HttpResponse.BodyHandlers.ofString());
    }

    private static Map<String, Object> result() {
        Map<String, Object> values = new LinkedHashMap<>();
        values.put("client_error_summary", List.of());
        values.put("client_error_other_count", 0L);
        values.put("throughput_per_second", 1.0);
        values.put("bandwidth_mb_s", 1.0);
        values.put("latency_mean_ms", 1.0);
        values.put("latency_p95_ms", 1.0);
        values.put("latency_p99_ms", 1.0);
        values.put("client_cpu_percent", 1.0);
        values.put("client_memory_mb", 1.0);
        values.put("server_cpu_percent", 1.0);
        values.put("server_memory_mb", 1.0);
        return values;
    }

    private static int freePort() throws IOException {
        try (ServerSocket socket = new ServerSocket(0)) {
            return socket.getLocalPort();
        }
    }

    private static void deleteTree(Path root) throws IOException {
        try (var paths = Files.walk(root)) {
            for (Path path : paths.sorted((left, right) -> right.compareTo(left)).toList()) {
                Files.delete(path);
            }
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
