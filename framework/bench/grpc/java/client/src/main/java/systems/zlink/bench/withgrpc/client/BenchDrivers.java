/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import systems.zlink.bench.withgrpc.shared.BenchHttpApplication;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;
import systems.zlink.bench.withgrpc.shared.MeasurementTiming;
import systems.zlink.contracts.sockets.SubmitResult;

import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicLongArray;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicReferenceArray;
import java.util.concurrent.locks.LockSupport;

/** Pattern-to-logical-stream implementation shared by the Java and Kotlin sources. */
public final class BenchDrivers {
    // Maximum wait before the completion poll returns to recheck completion or its deadline.
    private static final long COMPLETION_POLL_INTERVAL_NS = 50_000_000L;
    private static final int SUBMISSIONS_PER_YIELD = 256;
    private static final long DRAIN_POLL_INTERVAL_MS = 1;

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

    public Map<String, Object> runWarmup(
            BenchHttpApplication.Trigger trigger, BenchOperation operation) throws Exception {
        Map<String, Object> result = runPhase(trigger, operation);
        warmupAbandoned = ((Number) result.get("abandoned")).longValue();
        if (warmupAbandoned != 0) {
            System.err.println(
                    "warmup left " + warmupAbandoned + " operations abandoned (recorded)");
        }
        return result;
    }

    public Map<String, Object> runActive(
            BenchHttpApplication.Trigger trigger, BenchOperation operation) throws Exception {
        return runPhase(trigger, operation);
    }

    private Map<String, Object> runPhase(
            BenchHttpApplication.Trigger trigger, BenchOperation operation) throws Exception {
        validateTrigger(trigger);
        stats.reset(options.targetStatsUrl);
        ClientResources resources = new ClientResources();
        long startedAt = BenchMetricHeader.nowNs();
        long deadline = startedAt + trigger.durationMs() * 1_000_000L;
        SourceMetrics activeMetrics = new SourceMetrics(options.latencySampleLimit, deadline);
        metrics = activeMetrics;
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
                                ClientResources.Usage usage = resources.finish(deadline - startedAt);
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
                        "warmup".equals(trigger.phase())
                                ? BenchMetricHeader.PHASE_WARMUP
                                : BenchMetricHeader.PHASE_ACTIVE,
                        activeMetrics,
                        resources);
        activeMetrics.freezeTerminal();
        ActiveSnapshot active = activeAtClose.get();
        MeasurementTiming.window(trigger, startedAt, deadline);
        ClientResources.Usage usage = active.usage();
        StatsClient.ServerSnapshot boundary = active.target();
        StatsClient.ServerSnapshot target = stats.stats(options.targetStatsUrl);
        Latencies.Summary latency = activeMetrics.latencySummary();
        long rateCount = send ? boundary.received() : activeMetrics.activeCompleted.get();
        double seconds = (deadline - startedAt) / 1_000_000_000.0;
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
        result.put("active_started_at_ns", startedAt);
        result.put("active_ended_at_ns", deadline);
        result.put("active_elapsed_ms", (deadline - startedAt) / 1_000_000L);
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
        int streamCount = switch (trigger.pattern()) {
            case "request-backpressure", "send-saturation" -> trigger.submitConcurrency();
            case "request-serial", "request-window" -> 1;
            default -> throw new IllegalArgumentException("unknown pattern " + trigger.pattern());
        };
        if (!"request-serial".equals(trigger.pattern())
                && operation instanceof RawStack.RawOperation raw) {
            if ("send-saturation".equals(trigger.pattern())
                    && raw instanceof RawStack.RawSendOperation send) {
                runRawSendSaturation(trigger, send, streamCount, headerPhase, source, resources);
                return streamCount;
            }
            runRaw(trigger, raw, streamCount, headerPhase, source, resources);
            return streamCount;
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
                runBackpressure(trigger, operation, streamCount, headerPhase, source, resources);
                yield streamCount;
            }
            case "send-saturation" -> {
                runWorkers(
                        streamCount,
                        trigger,
                        operation,
                        headerPhase,
                        source,
                        resources);
                yield streamCount;
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
            int count,
            byte phase,
            SourceMetrics source,
            ClientResources resources)
            throws InterruptedException {
        List<SourceMetrics> streams = source.createStreams(count);
        List<Thread> workers = new ArrayList<>(count);
        AtomicBoolean pollerRunning = new AtomicBoolean(true);
        AtomicReference<Throwable> pollerFailure = new AtomicReference<>();
        Thread completionPoller = Thread.ofPlatform().name("bench-completion-poller").start(() -> {
            while (pollerRunning.get()) {
                try {
                    operation.pollCompletions(Duration.ofMillis(20));
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    break;
                } catch (Throwable error) {
                    pollerFailure.compareAndSet(null, error);
                    break;
                }
            }
        });
        Set<CompletableFuture<Void>> pendingAll = ConcurrentHashMap.newKeySet();
        for (int index = 0; index < count; index++) {
            final int stream = index;
            workers.add(Thread.ofPlatform().name("bench-stream-" + index).start(() -> {
                SourceMetrics own = streams.get(stream);
                long deadline = own.activeDeadline;
                long sequence = 0;
                long cpuStart = resources.beginSubmit();
                Set<CompletableFuture<Void>> pending = ConcurrentHashMap.newKeySet();
                CompletableFuture<Void> admissionPending = null;
                while (BenchMetricHeader.nowNs() < deadline) {
                    if (admissionPending != null) {
                        try {
                            admissionPending.get(Math.max(1L,
                                    Math.min(deadline - BenchMetricHeader.nowNs(),
                                            COMPLETION_POLL_INTERVAL_NS)), TimeUnit.NANOSECONDS);
                        } catch (ExecutionException failed) {
                            // The request completion callback owns error accounting.
                        } catch (TimeoutException stillBlocked) {
                            continue;
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            break;
                        }
                        admissionPending = null;
                    }
                    long started = own.begin();
                    RawStack.RawSubmission submission;
                    try {
                        submission = operation.submitRaw(trigger.payloadBytes(), phase,
                                sequence(stream, sequence++));
                    } catch (RuntimeException error) {
                        own.complete(started, error);
                        continue;
                    }
                    if (submission.result() != SubmitResult.OK
                            && submission.result() != SubmitResult.BACKPRESSURED) {
                        own.complete(started, new IllegalStateException(
                                "raw submit returned " + submission.result()));
                        continue;
                    }
                    CompletableFuture<Void> completion = submission.completion();
                    pending.add(completion);
                    pendingAll.add(completion);
                    completion.whenComplete((ignored, error) -> {
                        own.complete(started, error);
                        pending.remove(completion);
                        pendingAll.remove(completion);
                    });
                    if (submission.result() == SubmitResult.BACKPRESSURED) {
                        admissionPending = submission.admitted();
                    }
                }
                resources.addSubmitCpuNs(ClientResources.currentThreadCpuNs() - cpuStart);
                List<CompletableFuture<Void>> outstanding = new ArrayList<>(pending);
                if (admissionPending != null) outstanding.add(admissionPending);
                if (!outstanding.isEmpty()) {
                    try {
                        CompletableFuture.allOf(outstanding.toArray(CompletableFuture[]::new))
                                .get(options.drainBoundMs, TimeUnit.MILLISECONDS);
                    } catch (ExecutionException | TimeoutException ignored) {
                        // Per-operation callbacks own terminal accounting.
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                    }
                }
            }));
        }
        for (Thread worker : workers) worker.join();
        long drainDeadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(options.drainBoundMs);
        while (!pendingAll.isEmpty() && System.nanoTime() < drainDeadline) Thread.sleep(1);
        pollerRunning.set(false);
        completionPoller.join();
        if (pollerFailure.get() != null) {
            throw new IllegalStateException("raw completion poller failed", pollerFailure.get());
        }
        source.freezeTerminal();
    }

    private static long sequence(int stream, long ordinal) {
        return ((long) stream << 56) | (ordinal & 0x00ff_ffff_ffff_ffffL);
    }

    /** Each send stream submits on its own platform thread and owns its metrics. */
    private void runRawSendSaturation(
            BenchHttpApplication.Trigger trigger,
            RawStack.RawSendOperation operation,
            int count,
            byte phase,
            SourceMetrics source,
            ClientResources resources)
            throws InterruptedException {
        long deadline = source.activeDeadline;
        long drainDeadline =
                deadline + TimeUnit.MILLISECONDS.toNanos(options.drainBoundMs);
        AtomicInteger remainingWorkers = new AtomicInteger(count);
        CompletableFuture<Void> workersDone = new CompletableFuture<>();
        Set<CompletableFuture<Void>> pending = ConcurrentHashMap.newKeySet();
        List<Thread> workers = new ArrayList<>(count);
        List<SourceMetrics> streams = source.createStreams(count);

        for (int index = 0; index < count; index++) {
            final int stream = index;
            Thread worker =
                    Thread.ofPlatform()
                            .name("bench-stream-" + index)
                            .start(
                                    () -> {
                                        SourceMetrics own = streams.get(stream);
                                        long ordinal = 0;
                                        long cpuStart = resources.beginSubmit();
                                        try {
                                            while (BenchMetricHeader.nowNs() < deadline) {
                                                long value = sequence(stream, ordinal++);
                                                long started = own.begin();
                                                RawStack.PreparedSend prepared = null;
                                                try {
                                                    prepared =
                                                            operation.prepareRaw(
                                                                    trigger.payloadBytes(),
                                                                    phase,
                                                                    value);
                                                    if (drainDeadline - BenchMetricHeader.nowNs() <= 0) {
                                                        break;
                                                    }
                                                    RawStack.PreparedSend preparedForSubmit = prepared;
                                                    prepared = null;
                                                    RawStack.RawSubmission submission =
                                                            operation.submitRaw(preparedForSubmit);
                                                    if (submission.result() != SubmitResult.OK
                                                            && submission.result()
                                                                    != SubmitResult.BACKPRESSURED) {
                                                        own.complete(started, new IllegalStateException(
                                                                "raw send submit returned "
                                                                        + submission.result()));
                                                        continue;
                                                    }

                                                    CompletableFuture<Void> completion =
                                                            submission.completion();
                                                    pending.add(completion);
                                                    completion.whenComplete((ignored, error) -> {
                                                        own.complete(started, error);
                                                        pending.remove(completion);
                                                    });

                                                    if (submission.result()
                                                            == SubmitResult.BACKPRESSURED) {
                                                        long admissionWait = drainDeadline
                                                                - BenchMetricHeader.nowNs();
                                                        if (admissionWait > 0) {
                                                            try {
                                                                submission.admitted().get(
                                                                        admissionWait,
                                                                        TimeUnit.NANOSECONDS);
                                                            } catch (ExecutionException
                                                                    | TimeoutException
                                                                    | CancellationException ignored) {
                                                                // Unresolved admission is abandoned after the drain bound.
                                                            }
                                                        }
                                                    }
                                                } catch (InterruptedException interrupted) {
                                                    Thread.currentThread().interrupt();
                                                    break;
                                                } catch (RuntimeException error) {
                                                    own.complete(started, error);
                                                } finally {
                                                    if (prepared != null) {
                                                        prepared.close();
                                                    }
                                                }
                                            }
                                        } finally {
                                            resources.addSubmitCpuNs(
                                                    ClientResources.currentThreadCpuNs() - cpuStart);
                                            if (remainingWorkers.decrementAndGet() == 0) {
                                                workersDone.complete(null);
                                            }
                                        }
                                    });
            workers.add(worker);
        }

        while (!workersDone.isDone()) {
            long remaining = drainDeadline - BenchMetricHeader.nowNs();
            if (remaining <= 0) {
                break;
            }
            try {
                operation.await(
                        workersDone,
                        Duration.ofNanos(Math.min(COMPLETION_POLL_INTERVAL_NS, remaining)));
            } catch (ExecutionException impossible) {
                throw new IllegalStateException("send workers failed unexpectedly", impossible);
            }
        }
        for (Thread worker : workers) {
            worker.join();
        }

        List<CompletableFuture<Void>> outstanding = new ArrayList<>(pending);
        CompletableFuture<Void> settled =
                CompletableFuture.allOf(outstanding.toArray(CompletableFuture[]::new));
        long remaining = drainDeadline - BenchMetricHeader.nowNs();
        if (remaining > 0) {
            try {
                operation.await(settled, Duration.ofNanos(remaining));
            } catch (ExecutionException ignored) {
                // Per-send callbacks own success/error accounting.
            }
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
        List<Thread> workers = new ArrayList<>(count);
        List<SourceMetrics> streams = source.createStreams(count);
        for (int index = 0; index < count; index++) {
            final int stream = index;
            Thread worker =
                    Thread.ofPlatform()
                            .name("bench-stream-" + index)
                            .start(
                                    () -> {
                                        SourceMetrics own = streams.get(stream);
                                        long cpuStart = resources.beginSubmit();
                                        long ordinal = 0;
                                        while (BenchMetricHeader.nowNs() < deadline) {
                                            long value = sequence(stream, ordinal++);
                                            long started = own.begin();
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
                                                own.complete(started, null);
                                            } catch (Exception error) {
                                                own.complete(started, error);
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
        long cpuStart = resources.beginSubmit();
        long sequence = 0;
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
        long settleDeadline =
                System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(options.drainBoundMs);
        while (source.inFlight() > 0 && System.nanoTime() < settleDeadline) {
            Thread.sleep(DRAIN_POLL_INTERVAL_MS);
        }
        source.freezeTerminal();
    }

    private void runBackpressure(
            BenchHttpApplication.Trigger trigger,
            BenchOperation operation,
            int streamCount,
            byte phase,
            SourceMetrics source,
            ClientResources resources)
            throws InterruptedException {
        List<SourceMetrics> streams = source.createStreams(streamCount);
        List<Thread> workers = new ArrayList<>(streams.size());
        for (int index = 0; index < streams.size(); index++) {
            final int stream = index;
            workers.add(
                    Thread.ofPlatform()
                            .name("bench-stream-" + index)
                            .start(
                                    () -> {
                                        SourceMetrics own = streams.get(stream);
                                        long deadline = own.activeDeadline;
                                        long ordinal = 0;
                                        int issuedSinceYield = 0;
                                        long cpuStart = resources.beginSubmit();
                                        Set<CompletableFuture<Void>> pending =
                                                ConcurrentHashMap.newKeySet();
                                        BenchRequestOperation requests =
                                                operation instanceof BenchRequestOperation requestOperation
                                                        ? requestOperation
                                                        : null;
                                        while (BenchMetricHeader.nowNs() < deadline) {
                                            long started = own.begin();
                                            CompletableFuture<Void> future;
                                            BenchRequestOperation.Submission submitted;
                                            long requestSequence = sequence(stream, ordinal++);
                                            try {
                                                submitted = requests != null
                                                        ? requests.submit(trigger.payloadBytes(), phase,
                                                                requestSequence)
                                                        : null;
                                                future = submitted == null
                                                        ? operation.invoke(trigger.payloadBytes(), phase,
                                                                requestSequence)
                                                        : submitted.reply();
                                            } catch (Throwable error) {
                                                own.complete(started, error);
                                                continue;
                                            }
                                            pending.add(future);
                                            future.whenComplete((ignored, error) -> {
                                                own.complete(started, error);
                                                pending.remove(future);
                                            });
                                            if (submitted != null) {
                                                try {
                                                    operation.await(submitted.admitted(),
                                                            Duration.ofNanos(Math.max(0,
                                                                    deadline - BenchMetricHeader.nowNs())));
                                                } catch (ExecutionException
                                                        | CancellationException admissionFailure) {
                                                    // The reply callback owns terminal error accounting.
                                                } catch (InterruptedException interrupted) {
                                                    Thread.currentThread().interrupt();
                                                    break;
                                                }
                                            }
                                            if (++issuedSinceYield == SUBMISSIONS_PER_YIELD) {
                                                issuedSinceYield = 0;
                                                Thread.yield();
                                            }
                                        }
                                        resources.addSubmitCpuNs(
                                                ClientResources.currentThreadCpuNs() - cpuStart);
                                        long settleDeadline = System.nanoTime()
                                                + TimeUnit.MILLISECONDS.toNanos(options.drainBoundMs);
                                        while (!pending.isEmpty()
                                                && System.nanoTime() < settleDeadline) {
                                            try {
                                                Thread.sleep(DRAIN_POLL_INTERVAL_MS);
                                            } catch (InterruptedException interrupted) {
                                                Thread.currentThread().interrupt();
                                                break;
                                            }
                                        }
                                    }));
        }
        for (Thread worker : workers) worker.join();
        source.freezeTerminal();
    }

    private void validateTrigger(BenchHttpApplication.Trigger trigger) {
        if (!options.runIdText.equals(trigger.runId())
                || !options.scenario.equals(trigger.pattern())
                || options.payloadSizes.get(0) != trigger.payloadBytes()
                || trigger.requestWindow() != options.requestWindow
                || trigger.submitConcurrency() != options.submitConcurrency) {
            throw new IllegalArgumentException("trigger does not match the configured cell");
        }
    }

    static final class SourceMetrics {
        private static final int ERROR_KIND_LIMIT = 8;
        private static final int ERROR_MESSAGE_LIMIT = 200;
        private final AtomicLong submitted = new AtomicLong();
        private final AtomicLong completed = new AtomicLong();
        private final AtomicLong activeCompleted = new AtomicLong();
        private final long activeDeadline;
        private final AtomicLong errors = new AtomicLong();
        private final AtomicInteger inFlight = new AtomicInteger();
        private final AtomicLong peakInFlight = new AtomicLong();
        private final AtomicReferenceArray<ErrorKey> errorKeys =
                new AtomicReferenceArray<>(ERROR_KIND_LIMIT);
        private final AtomicLongArray errorCounts = new AtomicLongArray(ERROR_KIND_LIMIT);
        private final AtomicLong otherErrors = new AtomicLong();
        private final AtomicReference<TerminalSnapshot> terminal = new AtomicReference<>();
        private final Latencies latencies;
        private volatile List<SourceMetrics> streams = List.of();
        private volatile Latencies.Summary mergedLatency;

        SourceMetrics(int sampleLimit, long activeDeadline) {
            latencies = new Latencies(sampleLimit);
            this.activeDeadline = activeDeadline;
        }

        List<SourceMetrics> createStreams(int count) {
            if (!streams.isEmpty()) {
                throw new IllegalStateException("streams already created");
            }
            List<SourceMetrics> created = new ArrayList<>(count);
            for (int i = 0; i < count; i++) {
                SourceMetrics stream = new SourceMetrics(latencies.capacity(), activeDeadline);
                created.add(stream);
            }
            streams = List.copyOf(created);
            return streams;
        }

        long begin() {
            submitted.incrementAndGet();
            int depth = inFlight.incrementAndGet();
            peakInFlight.accumulateAndGet(depth, Math::max);
            return BenchMetricHeader.nowNs();
        }

        private void complete(long started, Throwable error) {
            complete(started, error, BenchMetricHeader.nowNs());
        }

        void complete(long started, Throwable error, long now) {
            if (error == null) {
                completed.incrementAndGet();
            } else {
                errors.incrementAndGet();
                recordError(error);
            }
            if (now < activeDeadline) {
                if (error == null) {
                    activeCompleted.incrementAndGet();
                }
                latencies.add((now - started) / 1000.0);
            }
            inFlight.decrementAndGet();
        }

        private void recordError(Throwable original) {
            Throwable error = unwrap(original);
            String type = error.getClass().getName();
            String message =
                    String.valueOf(error.getMessage()).replace('\r', ' ').replace('\n', ' ');
            if (message.length() > ERROR_MESSAGE_LIMIT) {
                message = message.substring(0, ERROR_MESSAGE_LIMIT);
            }
            ErrorKey key = new ErrorKey(type, message);
            int start = Math.floorMod(key.hashCode(), ERROR_KIND_LIMIT);
            for (; ; ) {
                boolean retry = false;
                for (int offset = 0; offset < ERROR_KIND_LIMIT; offset++) {
                    int index = (start + offset) % ERROR_KIND_LIMIT;
                    ErrorKey current = errorKeys.get(index);
                    if (key.equals(current)) {
                        errorCounts.incrementAndGet(index);
                        return;
                    }
                    if (current == null) {
                        if (errorKeys.compareAndSet(index, null, key)) {
                            errorCounts.incrementAndGet(index);
                            return;
                        }
                        retry = true;
                        break;
                    }
                }
                if (!retry) {
                    otherErrors.incrementAndGet();
                    return;
                }
            }
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
            if (!streams.isEmpty()) {
                Map<ErrorKey, Long> totals = new LinkedHashMap<>();
                for (SourceMetrics stream : streams) {
                    for (ErrorCount count : stream.errorCountsSnapshot()) {
                        totals.merge(count.key(), count.count(), Long::sum);
                    }
                }
                return errorMaps(totals);
            }
            TerminalSnapshot current = terminal.get();
            List<ErrorCount> counts = current == null ? errorCountsSnapshot() : current.summary();
            Map<ErrorKey, Long> totals = new LinkedHashMap<>();
            for (ErrorCount count : counts) totals.put(count.key(), count.count());
            return errorMaps(totals);
        }

        private static List<Map<String, Object>> errorMaps(Map<ErrorKey, Long> totals) {
            List<Map<String, Object>> summary = new ArrayList<>();
            for (Map.Entry<ErrorKey, Long> entry : totals.entrySet()) {
                summary.add(Map.of("type", entry.getKey().type(), "message",
                        entry.getKey().message(), "count", entry.getValue()));
            }
            return summary;
        }

        private List<ErrorCount> errorCountsSnapshot() {
            List<ErrorCount> summary = new ArrayList<>();
            for (int index = 0; index < ERROR_KIND_LIMIT; index++) {
                ErrorKey key = errorKeys.get(index);
                if (key == null) {
                    continue;
                }
                long count = errorCounts.get(index);
                if (count > 0) {
                    summary.add(new ErrorCount(key, count));
                }
            }
            return List.copyOf(summary);
        }

        void freezeTerminal() {
            if (terminal.get() != null) {
                return;
            }
            if (!streams.isEmpty()) {
                Map<ErrorKey, Long> merged = new LinkedHashMap<>();
                long frozenCompleted = 0;
                long frozenErrors = 0;
                long frozenAbandoned = 0;
                long frozenOtherErrors = 0;
                long frozenSubmitted = 0;
                long frozenActiveCompleted = 0;
                long frozenPeakInFlight = 0;
                for (SourceMetrics stream : streams) {
                    stream.freezeTerminal();
                    TerminalSnapshot current = stream.terminal.get();
                    frozenCompleted += current.completed();
                    frozenErrors += current.errors();
                    frozenAbandoned += current.abandoned();
                    frozenOtherErrors += current.otherErrors();
                    frozenSubmitted += stream.submitted.get();
                    frozenActiveCompleted += stream.activeCompleted.get();
                    frozenPeakInFlight += stream.peakInFlight.get();
                    for (ErrorCount count : current.summary()) merged.merge(
                            count.key(), count.count(), Long::sum);
                    latencies.merge(stream.latencies);
                }
                List<ErrorCount> summary = merged.entrySet().stream()
                        .map(entry -> new ErrorCount(entry.getKey(), entry.getValue())).toList();
                errors.set(frozenErrors);
                completed.set(frozenCompleted);
                activeCompleted.set(frozenActiveCompleted);
                submitted.set(frozenSubmitted);
                otherErrors.set(frozenOtherErrors);
                peakInFlight.set(frozenPeakInFlight);
                terminal.set(new TerminalSnapshot(frozenCompleted, frozenErrors, summary,
                        frozenOtherErrors, frozenAbandoned));
                mergedLatency = latencies.summary();
                return;
            }
            long frozenCompleted = completed.get();
            List<ErrorCount> summary = errorCountsSnapshot();
            long frozenOtherErrors = otherErrors.get();
            long frozenErrors = frozenOtherErrors;
            for (ErrorCount count : summary) {
                frozenErrors += count.count();
            }
            terminal.compareAndSet(
                    null,
                    new TerminalSnapshot(
                            frozenCompleted,
                            frozenErrors,
                            summary,
                            frozenOtherErrors,
                            submitted() - frozenCompleted - frozenErrors));
        }

        private BenchHttpApplication.Counters snapshot() {
            if (!streams.isEmpty() && terminal.get() == null) {
                long submitted = 0, completed = 0, errors = 0;
                for (SourceMetrics stream : streams) {
                    submitted += stream.submitted();
                    completed += stream.completed();
                    errors += stream.errors();
                }
                return new BenchHttpApplication.Counters(
                        submitted, completed, errors, aggregateInFlight(), peakInFlight());
            }
            TerminalSnapshot current = terminal.get();
            return new BenchHttpApplication.Counters(
                    submitted(),
                    current == null ? completed.get() : current.completed(),
                    current == null ? errors.get() : current.errors(),
                    current == null ? aggregateInFlight() : current.abandoned(),
                    peakInFlight());
        }

        private long submitted() {
            if (!streams.isEmpty() && terminal.get() == null) {
                return streams.stream().mapToLong(SourceMetrics::submitted).sum();
            }
            return submitted.get();
        }

        long completed() {
            if (!streams.isEmpty() && terminal.get() == null) {
                return streams.stream().mapToLong(SourceMetrics::completed).sum();
            }
            TerminalSnapshot current = terminal.get();
            return current == null ? completed.get() : current.completed();
        }

        long errors() {
            if (!streams.isEmpty() && terminal.get() == null) {
                return streams.stream().mapToLong(SourceMetrics::errors).sum();
            }
            TerminalSnapshot current = terminal.get();
            return current == null ? errors.get() : current.errors();
        }

        long inFlight() {
            if (!streams.isEmpty() && terminal.get() == null) {
                return streams.stream().mapToLong(SourceMetrics::inFlight).sum();
            }
            TerminalSnapshot current = terminal.get();
            return current == null ? inFlight.get() : current.abandoned();
        }

        long peakInFlight() {
            if (!streams.isEmpty() && terminal.get() == null) {
                return streams.stream().mapToLong(stream -> stream.peakInFlight.get()).sum();
            }
            return peakInFlight.get();
        }

        private int aggregateInFlight() {
            if (!streams.isEmpty()) {
                return streams.stream().mapToInt(stream -> stream.inFlight.get()).sum();
            }
            return inFlight.get();
        }

        long abandoned() {
            TerminalSnapshot current = terminal.get();
            return current == null ? 0 : current.abandoned();
        }

        private long otherErrors() {
            if (!streams.isEmpty() && terminal.get() == null) {
                return streams.stream().mapToLong(SourceMetrics::otherErrors).sum();
            }
            TerminalSnapshot current = terminal.get();
            return current == null ? otherErrors.get() : current.otherErrors();
        }

        private Latencies.Summary latencySummary() {
            if (mergedLatency != null) return mergedLatency;
            if (!streams.isEmpty() && terminal.get() == null) {
                return streams.stream().map(SourceMetrics::latencySummary)
                        .reduce(new Latencies.Summary(0, 0, 0, 0), (left, right) ->
                                new Latencies.Summary(0, 0, 0, left.count() + right.count()));
            }
            return latencies.summary();
        }

        private record ErrorKey(String type, String message) {}

        private record ErrorCount(ErrorKey key, long count) {}

        private record TerminalSnapshot(
                long completed,
                long errors,
                List<ErrorCount> summary,
                long otherErrors,
                long abandoned) {}
    }
}
