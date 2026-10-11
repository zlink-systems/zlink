package systems.zlink.bench.withgrpc.client;

import com.sun.net.httpserver.HttpServer;

import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;

import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;

/** A reply completed by drain remains a correctness count, not an active operation. */
public final class ActiveBoundaryTest {
    public static void main(String[] args) throws Exception {
        Class<?> source = BenchDrivers.SourceMetrics.class;
        for (var constructor : source.getDeclaredConstructors()) {
            if (constructor.getParameterCount() != 2) {
                throw new AssertionError("SourceMetrics must require a measurement deadline");
            }
        }
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
