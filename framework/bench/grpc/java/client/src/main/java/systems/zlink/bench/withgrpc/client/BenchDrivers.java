/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;

/** Pattern-to-logical-stream implementation shared by the Java and Kotlin sources. */
public final class BenchDrivers {
    private final BenchOptions options;
    private final StatsClient stats = new StatsClient();
    private volatile SourceMetrics metrics = new SourceMetrics(1);

    public BenchDrivers(BenchOptions options) {
        this.options = options;
    }

    /** Compatibility constructor retained for the Kotlin module's compiled call site. */
    public BenchDrivers(BenchOptions options, StatsClient ignored) {
        this(options);
    }

    public BenchHttpApplication.Counters counters() {
        return metrics.snapshot();
    }

    public void waitForRouteReady(BenchOperation operation, int payloadSize) throws Exception {
        long deadline = System.nanoTime() + options.routeReadyMs * 1_000_000L;
        Exception last = null;
        while (System.nanoTime() < deadline) {
            try {
                operation.invoke(payloadSize, BenchMetricHeader.PHASE_WARMUP, 0)
                    .get(options.requestTimeoutMs, TimeUnit.MILLISECONDS);
                return;
            } catch (Exception error) {
                last = error;
                Thread.sleep(20);
            }
        }
        throw new IllegalStateException("route not ready within " + options.routeReadyMs
            + "ms: " + last, last);
    }

    public void runWarmup(BenchHttpApplication.Trigger trigger, BenchOperation operation)
        throws Exception {
        validateTrigger(trigger);
        SourceMetrics warmupMetrics = new SourceMetrics(options.latencySampleLimit);
        metrics = warmupMetrics;
        runPattern(trigger, operation, BenchMetricHeader.PHASE_WARMUP, warmupMetrics, null);
        if (warmupMetrics.abandoned() != 0) {
            throw new IllegalStateException(
                "warmup left " + warmupMetrics.abandoned() + " operations abandoned");
        }
    }

    public Map<String, Object> runActive(
        BenchHttpApplication.Trigger trigger, BenchOperation operation) throws Exception {
        validateTrigger(trigger);
        stats.reset(options.targetStatsUrl);
        SourceMetrics activeMetrics = new SourceMetrics(options.latencySampleLimit);
        metrics = activeMetrics;
        ClientResources resources = new ClientResources();
        int submitParallelism = runPattern(
            trigger, operation, BenchMetricHeader.PHASE_ACTIVE, activeMetrics, resources);
        ClientResources.Usage usage = resources.finish();
        StatsClient.ServerSnapshot target = stats.stats(options.targetStatsUrl);
        Latencies.Summary latency = activeMetrics.latencySummary();
        boolean send = "send-saturation".equals(trigger.pattern());
        long rateCount = send ? target.received() : activeMetrics.completed();
        double seconds = Math.max(0.001, trigger.durationMs() / 1000.0);
        double throughput = rateCount / seconds;

        Map<String, Object> result = new LinkedHashMap<>();
        result.put("completed", activeMetrics.completed());
        result.put("submitted", activeMetrics.submitted());
        result.put("errors", activeMetrics.errors());
        result.put("server_errors", target.errors());
        result.put("throughput_per_second", throughput);
        result.put("bandwidth_mb_s", throughput * trigger.payloadBytes() / 1_000_000.0);
        result.put("latency_mean_ms",
            (send ? target.meanMicros() : latency.meanMicros()) / 1000.0);
        result.put("latency_p95_ms",
            (send ? target.p95Micros() : latency.p95Micros()) / 1000.0);
        result.put("latency_p99_ms",
            (send ? target.p99Micros() : latency.p99Micros()) / 1000.0);
        result.put("client_cpu_percent", usage.cpuPercent());
        result.put("client_memory_mb", usage.memoryMb());
        result.put("server_cpu_percent",
            target.cpuSeconds() / seconds / ClientResources.LOGICAL_CORES * 100.0);
        result.put("server_memory_mb", target.workingSetMb());
        result.put("client_saturation_metric", ClientResources.CLIENT_SATURATION_METRIC);
        result.put("jvm_thread_cores", usage.submitCores());
        result.put("client_parallelism_ceiling", submitParallelism);
        result.put("client_cores", usage.cores());
        result.put("jvm_all_thread_cores", usage.jvmAllThreadCores());
        result.put("logical_cores", ClientResources.LOGICAL_CORES);
        result.put("peak_in_flight", activeMetrics.peakInFlight());
        result.put("request_window",
            "request-window".equals(trigger.pattern()) ? trigger.requestWindow() : null);
        result.put("abandoned", activeMetrics.abandoned());
        result.put("latency_samples", latency.count());
        result.put("server_received_at_close", target.received());
        return result;
    }

    private int runPattern(
        BenchHttpApplication.Trigger trigger,
        BenchOperation operation,
        byte headerPhase,
        SourceMetrics source,
        ClientResources resources) throws Exception {
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
                runWorkers(trigger.sendConcurrency(), trigger, operation, headerPhase,
                    source, resources);
                yield trigger.sendConcurrency();
            }
            default -> throw new IllegalArgumentException("unknown pattern " + trigger.pattern());
        };
    }

    private void runWorkers(
        int count,
        BenchHttpApplication.Trigger trigger,
        BenchOperation operation,
        byte phase,
        SourceMetrics source,
        ClientResources resources) throws InterruptedException {
        long deadline = BenchMetricHeader.nowNs() + trigger.durationMs() * 1_000_000L;
        AtomicLong sequence = new AtomicLong();
        List<Thread> workers = new ArrayList<>(count);
        for (int index = 0; index < count; index++) {
            Thread worker = Thread.ofPlatform().name("bench-stream-" + index).start(() -> {
                long cpuStart = ClientResources.currentThreadCpuNs();
                while (BenchMetricHeader.nowNs() < deadline) {
                    long value = sequence.getAndIncrement();
                    long started = source.begin();
                    try {
                        operation.invoke(trigger.payloadBytes(), phase, value)
                            .get(options.requestTimeoutMs, TimeUnit.MILLISECONDS);
                        source.complete(started, true);
                    } catch (Exception error) {
                        source.complete(started, false);
                    }
                }
                if (resources != null) {
                    resources.addSubmitCpuNs(ClientResources.currentThreadCpuNs() - cpuStart);
                }
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
        ClientResources resources) throws InterruptedException {
        long deadline = BenchMetricHeader.nowNs() + trigger.durationMs() * 1_000_000L;
        Semaphore slots = new Semaphore(trigger.requestWindow());
        long sequence = 0;
        long cpuStart = ClientResources.currentThreadCpuNs();
        while (BenchMetricHeader.nowNs() < deadline) {
            if (!slots.tryAcquire(1, TimeUnit.MILLISECONDS)) {
                continue;
            }
            long started = source.begin();
            try {
                CompletableFuture<Void> future = operation.invoke(
                    trigger.payloadBytes(), phase, sequence++);
                future.whenComplete((ignored, error) -> {
                    source.complete(started, error == null);
                    slots.release();
                });
            } catch (Throwable error) {
                source.complete(started, false);
                slots.release();
            }
        }
        if (resources != null) {
            resources.addSubmitCpuNs(ClientResources.currentThreadCpuNs() - cpuStart);
        }
        long settleDeadline = System.nanoTime() + options.drainBoundMs * 1_000_000L;
        while (source.inFlight() > 0 && System.nanoTime() < settleDeadline) {
            Thread.sleep(1);
        }
        source.recordAbandoned(source.inFlight());
    }

    private void runBackpressure(
        BenchHttpApplication.Trigger trigger,
        BenchOperation operation,
        byte phase,
        SourceMetrics source,
        ClientResources resources) throws InterruptedException {
        long deadline = BenchMetricHeader.nowNs() + trigger.durationMs() * 1_000_000L;
        long sequence = 0;
        int issuedSinceYield = 0;
        long cpuStart = ClientResources.currentThreadCpuNs();
        Set<CompletableFuture<Void>> pending = ConcurrentHashMap.newKeySet();
        while (BenchMetricHeader.nowNs() < deadline) {
            long started = source.begin();
            CompletableFuture<Void> future;
            try {
                future = operation.invoke(trigger.payloadBytes(), phase, sequence++);
            } catch (Throwable error) {
                source.complete(started, false);
                continue;
            }
            pending.add(future);
            future.whenComplete((ignored, error) -> {
                source.complete(started, error == null);
                pending.remove(future);
            });
            if (++issuedSinceYield == 256) {
                issuedSinceYield = 0;
                Thread.yield();
            }
        }
        if (resources != null) {
            resources.addSubmitCpuNs(ClientResources.currentThreadCpuNs() - cpuStart);
        }
        long settleDeadline = System.nanoTime() + options.drainBoundMs * 1_000_000L;
        while (!pending.isEmpty() && System.nanoTime() < settleDeadline) {
            Thread.sleep(1);
        }
        source.recordAbandoned(source.inFlight());
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

    private static final class SourceMetrics {
        private final AtomicLong submitted = new AtomicLong();
        private final AtomicLong completed = new AtomicLong();
        private final AtomicLong errors = new AtomicLong();
        private final AtomicInteger inFlight = new AtomicInteger();
        private final AtomicInteger peakInFlight = new AtomicInteger();
        private final AtomicLong abandoned = new AtomicLong();
        private final Latencies latencies;

        private SourceMetrics(int sampleLimit) {
            latencies = new Latencies(sampleLimit);
        }

        private long begin() {
            submitted.incrementAndGet();
            int depth = inFlight.incrementAndGet();
            peakInFlight.accumulateAndGet(depth, Math::max);
            return BenchMetricHeader.nowNs();
        }

        private void complete(long started, boolean success) {
            if (success) {
                completed.incrementAndGet();
            } else {
                errors.incrementAndGet();
            }
            latencies.add((BenchMetricHeader.nowNs() - started) / 1000.0);
            inFlight.decrementAndGet();
        }

        private void recordAbandoned(long count) {
            abandoned.accumulateAndGet(count, Math::max);
        }

        private BenchHttpApplication.Counters snapshot() {
            return new BenchHttpApplication.Counters(
                submitted(), completed(), errors(), inFlight(), peakInFlight());
        }

        private long submitted() { return submitted.get(); }
        private long completed() { return completed.get(); }
        private long errors() { return errors.get(); }
        private long inFlight() { return inFlight.get(); }
        private long peakInFlight() { return peakInFlight.get(); }
        private long abandoned() { return abandoned.get(); }
        private Latencies.Summary latencySummary() { return latencies.summary(); }
    }
}
