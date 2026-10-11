/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;

import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;

/** Pattern-to-logical-stream implementation shared by the Java and Kotlin sources. */
public final class BenchDrivers {
    // Maximum wait before the completion poll returns to recheck completion or its deadline.
    private static final long COMPLETION_POLL_INTERVAL_NS = 50_000_000L;

    private final BenchOptions options;
    private final StatsClient stats = new StatsClient();
    private volatile SourceMetrics metrics;
    private long warmupAbandoned;

    public BenchDrivers(BenchOptions options) {
        this.options = options;
    }

    public BenchHttpApplication.Counters counters() {
        SourceMetrics current = metrics;
        return current == null ? BenchHttpApplication.Counters.empty() : current.snapshot();
    }

    public void waitForRouteReady(BenchOperation operation, int payloadSize) throws Exception {
        long deadline = System.nanoTime() + options.routeReadyMs * 1_000_000L;
        Exception last = null;
        while (System.nanoTime() < deadline) {
            try {
                CompletableFuture<Void> completion =
                        operation.invoke(payloadSize, BenchMetricHeader.PHASE_WARMUP, 0);
                if (!operation.await(completion, Duration.ofMillis(options.requestTimeoutMs))) {
                    throw new TimeoutException("route probe timed out");
                }
                return;
            } catch (Exception error) {
                last = error;
                Thread.sleep(20);
            }
        }
        throw new IllegalStateException(
                "route not ready within " + options.routeReadyMs + "ms: " + last, last);
    }

    public void runWarmup(BenchHttpApplication.Trigger trigger, BenchOperation operation)
            throws Exception {
        validateTrigger(trigger);
        long deadline = BenchMetricHeader.nowNs() + trigger.durationMs() * 1_000_000L;
        SourceMetrics warmupMetrics = new SourceMetrics(options.latencySampleLimit, deadline);
        metrics = warmupMetrics;
        runPattern(
                trigger,
                operation,
                BenchMetricHeader.PHASE_WARMUP,
                warmupMetrics,
                new ClientResources());
        // Operations still open when the warmup drain bound expires are a recorded observation
        // (spec 5.2: abandoned; the cell is excluded from the throughput judgement), not a phase
        // failure. The .NET and C++ sources record the same way; grpc-java's unbounded
        // request-backpressure warmup reaches this with about a million open calls.
        warmupMetrics.freezeTerminal();
        warmupAbandoned = warmupMetrics.abandoned();
        if (warmupAbandoned != 0) {
            System.err.println(
                    "warmup left " + warmupAbandoned + " operations abandoned (recorded)");
        }
    }

    public Map<String, Object> runActive(
            BenchHttpApplication.Trigger trigger, BenchOperation operation) throws Exception {
        validateTrigger(trigger);
        stats.reset(options.targetStatsUrl);
        long deadline = BenchMetricHeader.nowNs() + trigger.durationMs() * 1_000_000L;
        SourceMetrics activeMetrics = new SourceMetrics(options.latencySampleLimit, deadline);
        metrics = activeMetrics;
        ClientResources resources = new ClientResources();
        boolean send = "send-saturation".equals(trigger.pattern());
        CompletableFuture<ActiveSnapshot> activeAtClose = new CompletableFuture<>();
        Thread.ofVirtual()
                .start(
                        () -> {
                            try {
                                long remaining;
                                while ((remaining = deadline - BenchMetricHeader.nowNs()) > 0) {
                                    LockSupport.parkNanos(remaining);
                                }
                                ClientResources.Usage usage = resources.finish();
                                activeAtClose.complete(
                                        new ActiveSnapshot(
                                                usage, stats.stats(options.targetStatsUrl)));
                            } catch (Exception error) {
                                activeAtClose.completeExceptionally(error);
                            }
                        });
        int submitParallelism =
                runPattern(
                        trigger,
                        operation,
                        BenchMetricHeader.PHASE_ACTIVE,
                        activeMetrics,
                        resources);
        activeMetrics.freezeTerminal();
        ActiveSnapshot active = activeAtClose.get();
        ClientResources.Usage usage = active.usage();
        StatsClient.ServerSnapshot boundary = active.target();
        StatsClient.ServerSnapshot target = stats.stats(options.targetStatsUrl);
        Latencies.Summary latency = activeMetrics.latencySummary();
        long rateCount = send ? boundary.received() : activeMetrics.activeCompleted.get();
        double seconds = Math.max(0.001, trigger.durationMs() / 1000.0);
        double throughput = rateCount / seconds;

        Map<String, Object> result = new LinkedHashMap<>();
        result.put("completed", activeMetrics.completed());
        result.put("submitted", activeMetrics.submitted());
        result.put("errors", activeMetrics.errors());
        result.put("client_error_summary", activeMetrics.errorSummary());
        result.put("client_error_other_count", activeMetrics.otherErrors());
        result.put("server_errors", target.errors());
        result.put("throughput_per_second", throughput);
        result.put("bandwidth_mb_s", throughput * trigger.payloadBytes() / 1_000_000.0);
        result.put(
                "latency_mean_ms", (send ? boundary.meanMicros() : latency.meanMicros()) / 1000.0);
        result.put("latency_p95_ms", (send ? boundary.p95Micros() : latency.p95Micros()) / 1000.0);
        result.put("latency_p99_ms", (send ? boundary.p99Micros() : latency.p99Micros()) / 1000.0);
        result.put("client_cpu_percent", usage.cpuPercent());
        result.put("client_memory_mb", usage.memoryMb());
        result.put(
                "server_cpu_percent",
                boundary.cpuSeconds() / seconds / ClientResources.LOGICAL_CORES * 100.0);
        result.put("server_memory_mb", boundary.workingSetMb());
        result.put("client_saturation_metric", ClientResources.CLIENT_SATURATION_METRIC);
        result.put("jvm_thread_cores", usage.submitCores());
        result.put("client_parallelism_ceiling", submitParallelism);
        result.put("client_cores", usage.cores());
        result.put("jvm_all_thread_cores", usage.jvmAllThreadCores());
        result.put("logical_cores", ClientResources.LOGICAL_CORES);
        result.put("peak_in_flight", activeMetrics.peakInFlight());
        result.put(
                "request_window",
                "request-window".equals(trigger.pattern()) ? trigger.requestWindow() : null);
        result.put("abandoned", activeMetrics.abandoned());
        result.put("warmup_abandoned", warmupAbandoned);
        result.put("latency_samples", latency.count());
        result.put("completed_at_close", activeMetrics.activeCompleted.get());
        result.put("active_elapsed_ms", trigger.durationMs());
        result.put("server_received_at_close", boundary.received());
        return result;
    }

    private record ActiveSnapshot(ClientResources.Usage usage, StatsClient.ServerSnapshot target) {}

    private int runPattern(
            BenchHttpApplication.Trigger trigger,
            BenchOperation operation,
            byte headerPhase,
            SourceMetrics source,
            ClientResources resources)
            throws Exception {
        if (!"request-serial".equals(trigger.pattern())
                && operation instanceof RawStack.RawOperation raw) {
            runRaw(trigger, raw, headerPhase, source, resources);
            return 1;
        }
        return switch (trigger.pattern()) {
            case "request-serial" -> {
                runWorkers(1, trigger, operation, headerPhase, source, resources);
                yield 1;
            }
            case "request-window" -> {
                runWindow(trigger, operation, headerPhase, source, resources);
                yield 1;
            }
            case "request-backpressure" -> {
                runBackpressure(trigger, operation, headerPhase, source, resources);
                yield 1;
            }
            case "send-saturation" -> {
                runWorkers(
                        trigger.sendConcurrency(),
                        trigger,
                        operation,
                        headerPhase,
                        source,
                        resources);
                yield trigger.sendConcurrency();
            }
            default -> throw new IllegalArgumentException("unknown pattern " + trigger.pattern());
        };
    }

    /**
     * Request turns submit once, then let the public poller drain completions, as in
     * PerfMultiSocketReqRep at 31c5e4f7f0 (Issue #12's reference). Only admission backpressure
     * gates submission; reply depth has no cap.
     */
    private void runRaw(
            BenchHttpApplication.Trigger trigger,
            RawStack.RawOperation operation,
            byte phase,
            SourceMetrics source,
            ClientResources resources)
            throws InterruptedException {
        long deadline = source.activeDeadline;
        long sequence = 0;
        long cpuStart = resources.beginSubmit();
        Set<CompletableFuture<Void>> pending = ConcurrentHashMap.newKeySet();
        CompletableFuture<Void> admissionPending = null;
        while (BenchMetricHeader.nowNs() < deadline) {
            if (admissionPending != null) {
                try {
                    if (!operation.await(
                            admissionPending,
                            Duration.ofNanos(
                                    Math.min(
                                            COMPLETION_POLL_INTERVAL_NS,
                                            Math.max(1L, deadline - BenchMetricHeader.nowNs()))))) {
                        continue;
                    }
                } catch (ExecutionException failed) {
                    // The request's completion callback records the failure.
                }
                admissionPending = null;
            }
            long started = source.begin();
            RawStack.RawSubmission submission;
            try {
                submission = operation.submitRaw(trigger.payloadBytes(), phase, sequence++);
            } catch (RuntimeException error) {
                source.complete(started, error);
                continue;
            }

            CompletableFuture<Void> completion = submission.completion();
            pending.add(completion);
            completion.whenComplete(
                    (ignored, error) -> {
                        source.complete(started, error);
                        pending.remove(completion);
                    });

            if (submission.result() == systems.zlink.contracts.sockets.SubmitResult.BACKPRESSURED) {
                admissionPending = submission.admitted();
            } else if (submission.result() != systems.zlink.contracts.sockets.SubmitResult.OK) {
                throw new IllegalStateException("raw submit returned " + submission.result());
            }
            try {
                operation.await(completion, Duration.ZERO);
            } catch (ExecutionException failed) {
                // The completion callback records the failed reply.
            }
        }
        resources.addSubmitCpuNs(ClientResources.currentThreadCpuNs() - cpuStart);

        List<CompletableFuture<Void>> outstanding = new ArrayList<>(pending);
        if (admissionPending != null) {
            outstanding.add(admissionPending);
        }
        CompletableFuture<Void> settled =
                CompletableFuture.allOf(outstanding.toArray(CompletableFuture[]::new));
        try {
            operation.await(settled, Duration.ofMillis(options.drainBoundMs));
        } catch (ExecutionException ignored) {
            // Per-operation callbacks own success/error accounting. A drain that
            // is not done in time is recorded below as abandoned work.
        }
        source.freezeTerminal();
    }

    private void runWorkers(
            int count,
            BenchHttpApplication.Trigger trigger,
            BenchOperation operation,
            byte phase,
            SourceMetrics source,
            ClientResources resources)
            throws InterruptedException {
        long deadline = source.activeDeadline;
        AtomicLong sequence = new AtomicLong();
        List<Thread> workers = new ArrayList<>(count);
        for (int index = 0; index < count; index++) {
            Thread worker =
                    Thread.ofPlatform()
                            .name("bench-stream-" + index)
                            .start(
                                    () -> {
                                        long cpuStart = resources.beginSubmit();
                                        while (BenchMetricHeader.nowNs() < deadline) {
                                            long value = sequence.getAndIncrement();
                                            long started = source.begin();
                                            try {
                                                if (!operation.await(
                                                        operation.invoke(
                                                                trigger.payloadBytes(),
                                                                phase,
                                                                value),
                                                        Duration.ofMillis(
                                                                options.requestTimeoutMs))) {
                                                    throw new TimeoutException("request timed out");
                                                }
                                                source.complete(started, null);
                                            } catch (Exception error) {
                                                source.complete(started, error);
                                            }
                                        }
                                        resources.addSubmitCpuNs(
                                                ClientResources.currentThreadCpuNs() - cpuStart);
                                    });
            workers.add(worker);
        }
        for (Thread worker : workers) {
            worker.join();
        }
    }

    private void runWindow(
            BenchHttpApplication.Trigger trigger,
            BenchOperation operation,
            byte phase,
            SourceMetrics source,
            ClientResources resources)
            throws InterruptedException {
        long deadline = source.activeDeadline;
        Semaphore slots = new Semaphore(trigger.requestWindow());
        long sequence = 0;
        long cpuStart = resources.beginSubmit();
        while (BenchMetricHeader.nowNs() < deadline) {
            if (!slots.tryAcquire(1, TimeUnit.MILLISECONDS)) {
                continue;
            }
            long started = source.begin();
            try {
                CompletableFuture<Void> future =
                        operation.invoke(trigger.payloadBytes(), phase, sequence++);
                future.whenComplete(
                        (ignored, error) -> {
                            source.complete(started, error);
                            slots.release();
                        });
            } catch (Throwable error) {
                source.complete(started, error);
                slots.release();
            }
        }
        resources.addSubmitCpuNs(ClientResources.currentThreadCpuNs() - cpuStart);
        long settleDeadline = System.nanoTime() + options.drainBoundMs * 1_000_000L;
        while (source.inFlight() > 0 && System.nanoTime() < settleDeadline) {
            Thread.sleep(1);
        }
        source.freezeTerminal();
    }

    private void runBackpressure(
            BenchHttpApplication.Trigger trigger,
            BenchOperation operation,
            byte phase,
            SourceMetrics source,
            ClientResources resources)
            throws InterruptedException {
        long deadline = source.activeDeadline;
        long sequence = 0;
        int issuedSinceYield = 0;
        long cpuStart = resources.beginSubmit();
        Set<CompletableFuture<Void>> pending = ConcurrentHashMap.newKeySet();
        while (BenchMetricHeader.nowNs() < deadline) {
            long started = source.begin();
            CompletableFuture<Void> future;
            try {
                future = operation.invoke(trigger.payloadBytes(), phase, sequence++);
            } catch (Throwable error) {
                source.complete(started, error);
                continue;
            }
            pending.add(future);
            future.whenComplete(
                    (ignored, error) -> {
                        source.complete(started, error);
                        pending.remove(future);
                    });
            if (++issuedSinceYield == 256) {
                issuedSinceYield = 0;
                Thread.yield();
            }
        }
        resources.addSubmitCpuNs(ClientResources.currentThreadCpuNs() - cpuStart);
        long settleDeadline = System.nanoTime() + options.drainBoundMs * 1_000_000L;
        while (!pending.isEmpty() && System.nanoTime() < settleDeadline) {
            Thread.sleep(1);
        }
        source.freezeTerminal();
    }

    private void validateTrigger(BenchHttpApplication.Trigger trigger) {
        if (!options.runIdText.equals(trigger.runId())
                || !options.cellId.equals(trigger.cellId())
                || !options.scenario.equals(trigger.pattern())
                || options.payloadSizes.get(0) != trigger.payloadBytes()
                || trigger.requestWindow() != options.requestWindow
                || trigger.sendConcurrency() != options.sendConcurrency) {
            throw new IllegalArgumentException("trigger does not match the configured cell");
        }
    }

    static final class SourceMetrics {
        private static final int ERROR_KIND_LIMIT = 8;
        private static final int ERROR_MESSAGE_LIMIT = 200;
        private final AtomicLong submitted = new AtomicLong();
        private final AtomicLong activeCompleted = new AtomicLong();
        private final long activeDeadline;
        private final AtomicInteger peakInFlight = new AtomicInteger();
        private final AtomicReference<TerminalSnapshot> terminal =
                new AtomicReference<>(new TerminalSnapshot(0, 0, List.of(), 0, -1));
        private final Latencies latencies;

        SourceMetrics(int sampleLimit, long activeDeadline) {
            latencies = new Latencies(sampleLimit);
            this.activeDeadline = activeDeadline;
        }

        long begin() {
            submitted.incrementAndGet();
            int depth = (int) inFlight();
            peakInFlight.accumulateAndGet(depth, Math::max);
            return BenchMetricHeader.nowNs();
        }

        private void complete(long started, Throwable error) {
            complete(started, error, BenchMetricHeader.nowNs());
        }

        void complete(long started, Throwable error, long now) {
            ErrorKey key = error == null ? null : errorKey(error);
            for (; ; ) {
                TerminalSnapshot current = terminal.get();
                if (current.abandoned() >= 0) {
                    return;
                }
                TerminalSnapshot next = current.complete(key);
                if (terminal.compareAndSet(current, next)) {
                    break;
                }
            }
            if (now < activeDeadline) {
                if (error == null) {
                    activeCompleted.incrementAndGet();
                }
                latencies.add((now - started) / 1000.0);
            }
        }

        private static ErrorKey errorKey(Throwable original) {
            Throwable error = unwrap(original);
            String message =
                    String.valueOf(error.getMessage()).replace('\r', ' ').replace('\n', ' ');
            if (message.length() > ERROR_MESSAGE_LIMIT) {
                message = message.substring(0, ERROR_MESSAGE_LIMIT);
            }
            return new ErrorKey(error.getClass().getName(), message);
        }

        private static Throwable unwrap(Throwable error) {
            Throwable current = error;
            while ((current instanceof CompletionException || current instanceof ExecutionException)
                    && current.getCause() != null) {
                current = current.getCause();
            }
            return current;
        }

        List<Map<String, Object>> errorSummary() {
            List<Map<String, Object>> summary = new ArrayList<>();
            for (ErrorCount count : terminal.get().summary()) {
                summary.add(
                        Map.of(
                                "type",
                                count.key().type(),
                                "message",
                                count.key().message(),
                                "count",
                                count.count()));
            }
            return summary;
        }

        void freezeTerminal() {
            terminal.updateAndGet(
                    current ->
                            current.abandoned() >= 0
                                    ? current
                                    : new TerminalSnapshot(
                                            current.completed(),
                                            current.errors(),
                                            current.summary(),
                                            current.otherErrors(),
                                            submitted() - current.completed() - current.errors()));
        }

        private BenchHttpApplication.Counters snapshot() {
            return new BenchHttpApplication.Counters(
                    submitted(), completed(), errors(), inFlight(), peakInFlight());
        }

        private long submitted() {
            return submitted.get();
        }

        long completed() {
            return terminal.get().completed();
        }

        long errors() {
            return terminal.get().errors();
        }

        long inFlight() {
            TerminalSnapshot current = terminal.get();
            return current.abandoned() >= 0
                    ? current.abandoned()
                    : submitted() - current.completed() - current.errors();
        }

        private long peakInFlight() {
            return peakInFlight.get();
        }

        long abandoned() {
            return Math.max(0, terminal.get().abandoned());
        }

        private long otherErrors() {
            return terminal.get().otherErrors();
        }

        private Latencies.Summary latencySummary() {
            return latencies.summary();
        }

        private record ErrorKey(String type, String message) {}

        private record ErrorCount(ErrorKey key, long count) {}

        private record TerminalSnapshot(
                long completed,
                long errors,
                List<ErrorCount> summary,
                long otherErrors,
                long abandoned) {
            TerminalSnapshot complete(ErrorKey key) {
                if (key == null) {
                    return new TerminalSnapshot(
                            completed + 1, errors, summary, otherErrors, abandoned);
                }
                List<ErrorCount> updated = new ArrayList<>(summary);
                for (int index = 0; index < updated.size(); index++) {
                    ErrorCount count = updated.get(index);
                    if (count.key().equals(key)) {
                        updated.set(index, new ErrorCount(key, count.count() + 1));
                        return new TerminalSnapshot(
                                completed,
                                errors + 1,
                                List.copyOf(updated),
                                otherErrors,
                                abandoned);
                    }
                }
                if (updated.size() < ERROR_KIND_LIMIT) {
                    updated.add(new ErrorCount(key, 1));
                    return new TerminalSnapshot(
                            completed, errors + 1, List.copyOf(updated), otherErrors, abandoned);
                }
                return new TerminalSnapshot(
                        completed, errors + 1, summary, otherErrors + 1, abandoned);
            }
        }
    }
}
