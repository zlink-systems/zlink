/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.bench.withgrpc.client;

import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;
import systems.zlink.bench.withgrpc.shared.MeasurementTiming;
import java.nio.file.Files;
import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

/** Warmup exercises the same reset, close snapshot and accounting path as active. */
public final class PhasePathTest {
    public static void main(String[] args) throws Exception {
        AtomicInteger resets = new AtomicInteger();
        AtomicInteger snapshots = new AtomicInteger();
        HttpServer target = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        target.createContext("/bench/", exchange -> {
            if (exchange.getRequestURI().getPath().endsWith("reset")) resets.incrementAndGet();
            else snapshots.incrementAndGet();
            byte[] body = "{\"received\":0,\"errors\":0}".getBytes(StandardCharsets.UTF_8);
            exchange.sendResponseHeaders(200, body.length);
            try (var output = exchange.getResponseBody()) { output.write(body); }
        });
        target.start();
        try (Recording recording = new Recording()) {
            recording.enable("measure.MonotonicAnchor");
            recording.enable("measure.Phase");
            recording.start();
            MeasurementTiming.initialize();
            BenchOptions options = new BenchOptions(new String[] {
                "--scenario", "request-serial", "--implementation", "grpc-java", "--run-id", "phase-path",
                "--cell-id", "phase-path", "--trigger-url", "http://127.0.0.1:1", "--stats-url", "http://127.0.0.1:2",
                "--target-stats-url", "http://127.0.0.1:" + target.getAddress().getPort()
            });
            BenchDrivers driver = new BenchDrivers(options);
            driver.runWarmup(trigger("warmup"), (size, phase, seq) -> CompletableFuture.completedFuture(null));
            check(resets.get() == 1 && snapshots.get() == 2,
                "warmup omitted measurement control path: resets=" + resets + " snapshots=" + snapshots);
            var result = driver.runActive(trigger("active"), (size, phase, seq) -> CompletableFuture.completedFuture(null));
            check(resets.get() == 2 && snapshots.get() == 4,
                "active and warmup omitted reset or close snapshots");
            long start = ((Number) result.get("active_started_at_ns")).longValue();
            long end = ((Number) result.get("active_ended_at_ns")).longValue();
            check(end - start == 1_000_000L && ((Number) result.get("active_elapsed_ms")).longValue() == 1,
                "driver boundaries disagree with elapsed time: " + result);
            recording.stop();
            var path = Files.createTempFile("bench-phase-path-", ".jfr");
            try {
                recording.dump(path);
                var events = RecordingFile.readAllEvents(path);
                var windows = events.stream().filter(event -> event.getEventType().getName().equals("measure.Phase")
                    && event.getLong("durationNs") > 0).toList();
                check(windows.size() == 2, "warmup and active did not publish one window each");
                var active = windows.stream().filter(event -> "active".equals(event.getString("phase"))).findFirst().orElseThrow();
                check(active.getLong("startNs") == start && active.getLong("durationNs") == end - start
                    && active.getString("cellId").equals("phase-path-active"), "JFR and result boundaries disagree");
                var anchor = events.stream().filter(event -> event.getEventType().getName().equals("measure.MonotonicAnchor"))
                    .findFirst().orElseThrow();
                check(anchor.getLong("lowerNs") <= anchor.getLong("upperNs"), "invalid monotonic anchor interval");
            } finally { Files.delete(path); }
        } finally { target.stop(0); }
        System.out.println("PHASE_PATH_OK");
    }

    private static BenchHttpApplication.Trigger trigger(String phase) {
        return new BenchHttpApplication.Trigger("phase-path", "phase-path-" + phase, "request-serial", 1024, phase, 1, 100, 8);
    }
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
