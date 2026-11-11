package systems.zlink.bench.withgrpc.client;

import com.sun.net.httpserver.HttpServer;

import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;

import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.ConcurrentHashMap;

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
            var calls = new AtomicInteger();
            var violation = new CompletableFuture<String>();
            var admissions = new ConcurrentHashMap<Long, CompletableFuture<Void>>();
            var replies = new ConcurrentHashMap<Long, CompletableFuture<Void>>();
            var secondByStream = ConcurrentHashMap.<Long>newKeySet();
            BenchRequestOperation operation =
                    new BenchRequestOperation() {
                        @Override
                        public Submission submit(int size, byte phase, long sequence) {
                            calls.incrementAndGet();
                            long stream = sequence >>> 56;
                            long ordinal = sequence & 0x00ff_ffff_ffff_ffffL;
                            if (ordinal == 0) {
                                var admission = new CompletableFuture<Void>();
                                var reply = new CompletableFuture<Void>();
                                admissions.put(stream, admission);
                                replies.put(stream, reply);
                                return new Submission(admission, reply);
                            }
                            var admission = admissions.get(stream);
                            var reply = replies.get(stream);
                            if (admission == null || !admission.isDone())
                                violation.complete("stream advanced before admission");
                            if (ordinal == 1) {
                                if (reply == null || reply.isDone())
                                    violation.complete("stream waited for reply");
                                secondByStream.add(stream);
                                reply.complete(null);
                            }
                            return new Submission(CompletableFuture.completedFuture(null),
                                    CompletableFuture.completedFuture(null));
                        }

                        @Override
                        public boolean await(CompletableFuture<Void> completion, Duration timeout)
                                throws InterruptedException, ExecutionException {
                            if (admissions.containsValue(completion)) {
                                completion.complete(null);
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
                            100,
                            100,
                            8);
            var result = new BenchDrivers(options).runActive(trigger, operation);
            if (violation.isDone()) throw new AssertionError(violation.join());
            if (admissions.size() != 8 || secondByStream.size() != 8
                    || ((Number) result.get("completed")).longValue() < 16
                    || ((Number) result.get("errors")).longValue() != 0)
                throw new AssertionError(
                        "stream terminal accounting: " + result + " calls=" + calls);
            var failedCalls = new AtomicInteger();
            var failedByStream = ConcurrentHashMap.<Long>newKeySet();
            BenchRequestOperation refused =
                    (size, phase, sequence) -> {
                        failedCalls.incrementAndGet();
                        long stream = sequence >>> 56;
                        return failedByStream.add(stream)
                                    ? new BenchRequestOperation.Submission(
                                            CompletableFuture.failedFuture(
                                                    new IllegalStateException("admission refused")),
                                            CompletableFuture.failedFuture(
                                                    new IllegalStateException("admission refused")))
                                    : new BenchRequestOperation.Submission(
                                            new CompletableFuture<>(),
                                            CompletableFuture.completedFuture(null));
                    };
            var refusedResult = new BenchDrivers(options).runActive(trigger, refused);
            if (failedByStream.size() != 8
                    || ((Number) refusedResult.get("errors")).longValue() != 8)
                throw new AssertionError("admission failure counted twice: " + refusedResult);
        } finally {
            stats.stop(0);
        }
    }
}
