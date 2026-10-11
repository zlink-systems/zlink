package systems.zlink.bench.withgrpc.client;

import com.sun.net.httpserver.HttpServer;

import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;

import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;

/** A reply completed by drain remains a correctness count, not an active operation. */
public final class ActiveBoundaryTest {
    public static void main(String[] args) throws Exception {
        var metrics = new BenchDrivers.SourceMetrics(10, 10);
        long first = metrics.begin();
        long second = metrics.begin();
        long third = metrics.begin();
        metrics.complete(first, null, 1);
        metrics.freezeTerminal();
        metrics.complete(second, null, 20);
        metrics.complete(third, new IllegalStateException("late"), 21);
        if (metrics.completed() + metrics.errors() + metrics.abandoned() != 3
                || metrics.completed() != 1
                || metrics.errors() != 0
                || !metrics.errorSummary().isEmpty()) {
            throw new AssertionError(
                    "late terminal entered snapshot: "
                            + metrics.completed()
                            + "/"
                            + metrics.errors()
                            + "/"
                            + metrics.abandoned());
        }
        var errorMetrics = new BenchDrivers.SourceMetrics(10, 0);
        for (int index = 0; index < 10; index++) {
            errorMetrics.begin();
            errorMetrics.complete(0, new IllegalStateException("error-" + index), 20);
        }
        errorMetrics.begin();
        errorMetrics.complete(0, new IllegalStateException("error-0"), 20);
        errorMetrics.freezeTerminal();
        if (errorMetrics.errors() != 11
                || errorMetrics.errorSummary().size() != 8
                || errorMetrics.errorSummary().stream()
                                .mapToLong(entry -> ((Number) entry.get("count")).longValue())
                                .sum()
                        != 9)
            throw new AssertionError("bounded error summary lost terminal counts");
        var concurrent = new BenchDrivers.SourceMetrics(10, 0);
        for (int index = 0; index < 4000; index++) concurrent.begin();
        var failure = new IllegalStateException("early");
        Thread[] completions = new Thread[4];
        for (int index = 0; index < completions.length; index++) {
            completions[index] =
                    Thread.ofPlatform()
                            .start(
                                    () -> {
                                        for (int count = 0; count < 1000; count++)
                                            concurrent.complete(
                                                    0, count % 2 == 0 ? failure : null, 20);
                                    });
        }
        concurrent.freezeTerminal();
        for (Thread completion : completions) completion.join();
        long summarized =
                concurrent.errorSummary().stream()
                        .mapToLong(entry -> ((Number) entry.get("count")).longValue())
                        .sum();
        if (concurrent.completed() + concurrent.errors() + concurrent.abandoned() != 4000
                || summarized != concurrent.errors())
            throw new AssertionError("terminal counters and error summary diverged");
        HttpServer stats = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        stats.createContext(
                "/bench/",
                exchange -> {
                    byte[] body =
                            "{\"received\":1,\"activeMessages\":1,\"errors\":0}"
                                    .getBytes(StandardCharsets.UTF_8);
                    exchange.sendResponseHeaders(200, body.length);
                    exchange.getResponseBody().write(body);
                    exchange.close();
                });
        stats.start();
        try {
            BenchOptions options =
                    new BenchOptions(
                            new String[] {
                                "--scenario", "request-serial", "--implementation", "grpc-java",
                                "--run-id", "boundary", "--cell-id", "boundary",
                                "--trigger-url", "http://127.0.0.1:1", "--stats-url",
                                        "http://127.0.0.1:2",
                                "--target-stats-url",
                                        "http://127.0.0.1:" + stats.getAddress().getPort()
                            });
            BenchDrivers driver = new BenchDrivers(options);
            var trigger =
                    new BenchHttpApplication.Trigger(
                            "boundary", "boundary", "request-serial", 1024, "active", 20, 100, 8);
            var result =
                    driver.runActive(
                            trigger,
                            (size, phase, seq) ->
                                    CompletableFuture.runAsync(
                                            () -> {
                                                try {
                                                    Thread.sleep(100);
                                                } catch (InterruptedException error) {
                                                    Thread.currentThread().interrupt();
                                                    throw new IllegalStateException(error);
                                                }
                                            }));
            if (((Number) result.get("completed")).longValue() != 1
                    || ((Number) result.get("throughput_per_second")).doubleValue() != 0
                    || ((Number) result.get("latency_mean_ms")).doubleValue() != 0) {
                throw new AssertionError("drain reply entered active metrics: " + result);
            }
        } finally {
            stats.stop(0);
        }
    }
}
