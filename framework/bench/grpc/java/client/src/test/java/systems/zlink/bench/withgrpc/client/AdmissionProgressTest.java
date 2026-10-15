package systems.zlink.bench.withgrpc.client;

import com.sun.net.httpserver.HttpServer;

import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;

import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.atomic.AtomicInteger;

public final class AdmissionProgressTest {
    public static void main(String[] args) throws Exception {
        var stats = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        stats.createContext(
                "/bench/stats",
                exchange -> {
                    var body =
                            "{\"received\":2,\"activeMessages\":2,\"errors\":0}"
                                    .getBytes(StandardCharsets.UTF_8);
                    exchange.sendResponseHeaders(200, body.length);
                    exchange.getResponseBody().write(body);
                    exchange.close();
                });
        stats.createContext(
                "/bench/reset",
                exchange -> {
                    exchange.sendResponseHeaders(200, -1);
                    exchange.close();
                });
        stats.start();
        try {
            var options =
                    new BenchOptions(
                            new String[] {
                                "--scenario", "request-backpressure", "--implementation",
                                        "zlink-framework-java",
                                "--run-id", "admission", "--cell-id", "admission",
                                "--trigger-url", "http://127.0.0.1:1", "--stats-url",
                                        "http://127.0.0.1:2",
                                "--target-stats-url",
                                        "http://127.0.0.1:" + stats.getAddress().getPort()
                            });
            var firstAdmission = new CompletableFuture<Void>();
            var firstReply = new CompletableFuture<Void>();
            var calls = new AtomicInteger();
            var awaits = new AtomicInteger();
            var violation = new CompletableFuture<String>();
            BenchRequestOperation operation =
                    new BenchRequestOperation() {
                        @Override
                        public Submission submit(int size, byte phase, long sequence) {
                            int call = calls.incrementAndGet();
                            if (call == 1) return new Submission(firstAdmission, firstReply);
                            if (!firstAdmission.isDone())
                                violation.complete("next request preceded admission");
                            if (firstReply.isDone())
                                violation.complete("next request waited for reply");
                            firstReply.complete(null);
                            return new Submission(
                                    new CompletableFuture<>(),
                                    CompletableFuture.completedFuture(null));
                        }

                        @Override
                        public boolean await(CompletableFuture<Void> completion, Duration timeout)
                                throws InterruptedException, ExecutionException {
                            if (awaits.incrementAndGet() == 1) {
                                if (completion != firstAdmission)
                                    violation.complete("driver waited on reply");
                                firstAdmission.complete(null);
                                return true;
                            }
                            return BenchRequestOperation.super.await(completion, timeout);
                        }
                    };
            var trigger =
                    new BenchHttpApplication.Trigger(
                            "admission",
                            "admission",
                            "request-backpressure",
                            1024,
                            "active",
                            500,
                            100,
                            8);
            var result = new BenchDrivers(options).runActive(trigger, operation);
            if (violation.isDone()) throw new AssertionError(violation.join());
            if (calls.get() != 2
                    || ((Number) result.get("completed")).longValue() != 2
                    || ((Number) result.get("errors")).longValue() != 0)
                throw new AssertionError(
                        "request terminal accounting: " + result + " calls=" + calls);
            var failedCalls = new AtomicInteger();
            var failedTerminal =
                    CompletableFuture.<Void>failedFuture(
                            new IllegalStateException("admission refused"));
            BenchRequestOperation refused =
                    (size, phase, sequence) ->
                            failedCalls.incrementAndGet() == 1
                                    ? new BenchRequestOperation.Submission(
                                            failedTerminal, failedTerminal)
                                    : new BenchRequestOperation.Submission(
                                            new CompletableFuture<>(),
                                            CompletableFuture.completedFuture(null));
            var refusedResult = new BenchDrivers(options).runActive(trigger, refused);
            if (failedCalls.get() != 2
                    || ((Number) refusedResult.get("completed")).longValue() != 1
                    || ((Number) refusedResult.get("errors")).longValue() != 1)
                throw new AssertionError("admission failure counted twice: " + refusedResult);
        } finally {
            stats.stop(0);
        }
    }
}
