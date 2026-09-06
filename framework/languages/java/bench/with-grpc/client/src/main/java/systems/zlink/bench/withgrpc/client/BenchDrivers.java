/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.bench.withgrpc.shared.BenchMetricHeader;

/**
 * Pattern drivers shared by every implementation in the java row.
 *
 * <p>Each driver returns a cell record in the {@code with-grpc-cell-v1} shape the shared
 * aggregator reads (FB-021). Nothing here decides a verdict: medians, G5 and the
 * section 7.2 ratios belong to framework/bench/tools, never to a language harness.
 */
public final class BenchDrivers {
    private final BenchOptions options;
    private final StatsClient stats;

    public BenchDrivers(BenchOptions options, StatsClient stats) {
        this.options = options;
        this.stats = stats;
    }

    /**
     * Bounded route-readiness probe. A ROUTER addressing a peer by routing id fails
     * until that peer's id is in the local routing map, so every socket is probed
     * ONCE, before warmup. It never runs inside a measured window and it is not a
     * retry: the reset that opens the active phase happens after it returns.
     */
    public void waitForRouteReady(BenchOperation operation, int payloadSize)
        throws Exception {
        long deadline = System.nanoTime() + options.routeReadyMs * 1_000_000L;
        Exception last = null;
        while (System.nanoTime() < deadline) {
            try {
                operation.invoke(payloadSize, BenchMetricHeader.PHASE_WARMUP, 0L)
                    .get(options.routeReadyMs, TimeUnit.MILLISECONDS);
                return;
            } catch (Exception error) {
                last = error;
                Thread.sleep(20);
            }
        }
        throw new IllegalStateException(
            "route not ready within " + options.routeReadyMs + "ms: " + last, last);
    }

    /**
     * Close the warmup before the active window opens.
     *
     * <p>spec section 6 counts only payloads whose header says phase == active, so a server
     * still working through warmup backlog when the active window opens spends that
     * window consuming warmup messages and the cell reports a throughput near zero
     * while the stack is in fact busy. Measured here: a 0.5 s framework send warmup
     * left the server 5,483 messages behind, and every one of them was consumed
     * inside the active window.
     *
     * <p>This is the FB-008 settle contract applied at the warmup boundary rather than
     * the cell boundary, and it is bounded the same way. It does not change what is
     * measured -- it makes the active window contain the active phase.
     */
    private void closeWarmup(String statsUrl) throws Exception {
        StatsClient.DrainOutcome outcome = stats.waitForServerDrain(
            statsUrl, options.commandSettleMs, options.drainBoundMs);
        if (outcome.boundHit()) {
            System.err.printf(
                "[bench] warmup did not drain within %dms (observed %.0fms)%n",
                options.drainBoundMs, outcome.drainMs());
        }
        stats.reset(statsUrl);
    }

    // --- request-serial ----------------------------------------------------

    /** {@code request-serial}: one outstanding request, the next submitted after the reply. */
    public Map<String, Object> runRequestSerial(
        int payloadSize, String statsUrl, BenchOperation operation) throws Exception {
        List<Double> warmupSegments = warmup(
            payloadSize, operation, seconds -> serialLoop(
                payloadSize, operation, BenchMetricHeader.PHASE_WARMUP, seconds, null));
        closeWarmup(statsUrl);

        Latencies latencies = new Latencies(options.latencySampleLimit);
        ClientResources resources = new ClientResources();
        // The serial driver submits from this one thread: declared parallelism 1.
        long submitCpuStart = ClientResources.currentThreadCpuNs();
        Counts counts = serialLoop(payloadSize, operation, BenchMetricHeader.PHASE_ACTIVE,
            options.durationSeconds, latencies);
        resources.addSubmitCpuNs(ClientResources.currentThreadCpuNs() - submitCpuStart);
        ClientResources.Usage usage = resources.finish();
        StatsClient.ServerSnapshot server = stats.stats(statsUrl);

        return requestCell(payloadSize, usage, server, latencies, counts,
            1, 0, counts.completed, 1, warmupSegments);
    }

    private Counts serialLoop(
        int payloadSize, BenchOperation operation, byte phase, double seconds,
        Latencies latencies) {
        long completed = 0;
        long errors = 0;
        long sequence = 0;
        long startNs = BenchMetricHeader.nowNs();
        long until = startNs + (long) (seconds * 1e9);
        while (BenchMetricHeader.nowNs() < until) {
            long index = sequence++;
            long t0 = BenchMetricHeader.nowNs();
            try {
                operation.invoke(payloadSize, phase, index)
                    .get(options.requestTimeoutMs, TimeUnit.MILLISECONDS);
                completed++;
            } catch (Exception error) {
                errors++;
            }
            if (latencies != null) {
                latencies.add((BenchMetricHeader.nowNs() - t0) / 1000.0);
            }
        }
        return new Counts(completed, errors, completed);
    }

    // --- request-window ----------------------------------------------------

    /**
     * {@code request-window}: up to {@code request_window} outstanding requests.
     *
     * <p>Admission is a semaphore, not a scan over a pending list: FB-010 showed an
     * O(window) drain scan capping the submit rate so hard that the .NET raw row held
     * 8 requests against a configured window of 100. {@code peak_in_flight} and the
     * abandoned count are reported per cell (FB-017) because that pair is what
     * separates "the harness cannot fill the window" from "the stack only reaches this
     * depth"; without it a wrong premise survives.
     */
    public Map<String, Object> runRequestWindow(
        int payloadSize, String statsUrl, BenchOperation operation) throws Exception {
        List<Double> warmupSegments = warmup(
            payloadSize, operation, seconds -> windowLoop(
                payloadSize, operation, BenchMetricHeader.PHASE_WARMUP, seconds, null).counts);
        closeWarmup(statsUrl);

        Latencies latencies = new Latencies(options.latencySampleLimit);
        ClientResources resources = new ClientResources();
        // The window driver also submits from this one thread; the window is depth,
        // not thread parallelism. Declared parallelism 1.
        long submitCpuStart = ClientResources.currentThreadCpuNs();
        WindowOutcome outcome = windowLoop(payloadSize, operation,
            BenchMetricHeader.PHASE_ACTIVE, options.durationSeconds, latencies);
        resources.addSubmitCpuNs(ClientResources.currentThreadCpuNs() - submitCpuStart);
        ClientResources.Usage usage = resources.finish();
        StatsClient.ServerSnapshot server = stats.stats(statsUrl);

        return requestCell(payloadSize, usage, server, latencies, outcome.counts,
            outcome.peakInFlight, outcome.abandoned, outcome.completedAtClose, 1,
            warmupSegments);
    }

    private WindowOutcome windowLoop(
        int payloadSize, BenchOperation operation, byte phase, double seconds,
        Latencies latencies) {
        Semaphore slots = new Semaphore(options.requestWindow);
        AtomicInteger inFlight = new AtomicInteger();
        AtomicInteger peak = new AtomicInteger();
        AtomicLong completed = new AtomicLong();
        AtomicLong errors = new AtomicLong();
        long sequence = 0;
        long until = BenchMetricHeader.nowNs() + (long) (seconds * 1e9);

        while (BenchMetricHeader.nowNs() < until) {
            try {
                if (!slots.tryAcquire(1, TimeUnit.MILLISECONDS)) {
                    continue;
                }
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                break;
            }
            int depth = inFlight.incrementAndGet();
            peak.accumulateAndGet(depth, Math::max);
            long index = sequence++;
            long t0 = BenchMetricHeader.nowNs();
            operation.invoke(payloadSize, phase, index).whenComplete((value, error) -> {
                if (error == null) {
                    completed.incrementAndGet();
                } else {
                    errors.incrementAndGet();
                }
                if (latencies != null) {
                    latencies.add((BenchMetricHeader.nowNs() - t0) / 1000.0);
                }
                inFlight.decrementAndGet();
                slots.release();
            });
        }

        long completedAtClose = completed.get();
        // Settle the requests already in flight. Bounded; whatever is still
        // outstanding at the bound is counted as abandoned rather than dropped.
        long settleDeadline = System.nanoTime() + options.windowSettleMs * 1_000_000L;
        while (inFlight.get() > 0 && System.nanoTime() < settleDeadline) {
            try {
                Thread.sleep(1);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                break;
            }
        }
        int abandoned = inFlight.get();
        long errorTotal = errors.get() + abandoned;
        return new WindowOutcome(
            new Counts(completed.get(), errorTotal, completedAtClose),
            peak.get(), abandoned, completedAtClose);
    }

    // --- send-saturation ---------------------------------------------------

    /**
     * {@code send-saturation}. spec section 5 / G3: throughput is what the SERVER received
     * during the active phase.
     *
     * <p>FB-013: the snapshot is taken AT THE ACTIVE-WINDOW BOUNDARY. Reading it after
     * the drain counts messages that landed seconds after the window closed and reports
     * the client's submit rate as the server's consumption rate; on .NET that inflated
     * the framework row 4.2x and manufactured a 2.8x advantage over gRPC that vanished
     * once corrected. The drain still runs, purely as settle and contamination
     * detection (FB-008), and its observed time is reported per cell.
     */
    public Map<String, Object> runSendSaturation(
        int payloadSize, String statsUrl, BenchOperation operation) throws Exception {
        List<Double> warmupSegments = warmup(
            payloadSize, operation, seconds -> sendLoop(
                payloadSize, operation, BenchMetricHeader.PHASE_WARMUP, seconds, null).counts);
        closeWarmup(statsUrl);

        Latencies clientLatencies = new Latencies(options.latencySampleLimit);
        ClientResources resources = new ClientResources();
        SendOutcome outcome = sendLoop(payloadSize, operation,
            BenchMetricHeader.PHASE_ACTIVE, options.durationSeconds, clientLatencies,
            resources);
        ClientResources.Usage usage = resources.finish();
        StatsClient.ServerSnapshot boundary = stats.stats(statsUrl);
        StatsClient.DrainOutcome drain = stats.waitForServerDrain(
            statsUrl, options.commandSettleMs, options.drainBoundMs);
        StatsClient.ServerSnapshot postDrain =
            drain.snapshot() == null ? boundary : drain.snapshot();

        double seconds = Math.max(1e-9, options.durationSeconds);
        double throughput = boundary.activeMessages() / seconds;
        Latencies.Summary clientSummary = clientLatencies.summary();

        Map<String, Object> cell = new LinkedHashMap<>();
        cell.put("throughput_per_second", throughput);
        cell.put("bandwidth_mb_s", (throughput * payloadSize) / 1e6);
        // spec section 5: for send, the reported latency is the SERVER-side receive
        // latency computed from the header, not the client's submit-call duration.
        cell.put("latency_mean_ms", boundary.meanMicros() / 1000.0);
        cell.put("latency_p95_ms", boundary.p95Micros() / 1000.0);
        cell.put("latency_p99_ms", boundary.p99Micros() / 1000.0);
        putClientColumns(cell, usage);
        cell.put("server_cpu_percent",
            (boundary.cpuSeconds() / usage.elapsedSeconds() / ClientResources.LOGICAL_CORES)
                * 100.0);
        cell.put("server_memory_mb", boundary.workingSetMb());
        putDiagnostics(cell, usage, outcome.peakInFlight, options.sendConcurrency, 0,
            options.sendConcurrency, warmupSegments);
        cell.put("drain_ms", drain.drainMs());
        cell.put("drain_bound_hit", drain.boundHit());
        cell.put("server_received_at_close", boundary.activeMessages());
        cell.put("server_received_post_drain", postDrain.activeMessages());
        cell.put("server_received_any_phase_at_close", boundary.totalMessages());
        cell.put("errors", outcome.counts.errors);
        cell.put("submitted", outcome.counts.completed + outcome.counts.errors);
        cell.put("client_submit_latency_mean_ms", clientSummary.meanMicros() / 1000.0);
        cell.put("client_submit_latency_p95_ms", clientSummary.p95Micros() / 1000.0);
        cell.put("client_submit_latency_p99_ms", clientSummary.p99Micros() / 1000.0);
        return cell;
    }

    private SendOutcome sendLoop(
        int payloadSize, BenchOperation operation, byte phase, double seconds,
        Latencies latencies) {
        return sendLoop(payloadSize, operation, phase, seconds, latencies, null);
    }

    private SendOutcome sendLoop(
        int payloadSize, BenchOperation operation, byte phase, double seconds,
        Latencies latencies, ClientResources resources) {
        AtomicLong submitted = new AtomicLong();
        AtomicLong errors = new AtomicLong();
        AtomicInteger inFlight = new AtomicInteger();
        AtomicInteger peak = new AtomicInteger();
        long until = BenchMetricHeader.nowNs() + (long) (seconds * 1e9);

        List<Thread> workers = new ArrayList<>();
        for (int slot = 0; slot < options.sendConcurrency; slot++) {
            Thread worker = new Thread(() -> {
                long workerCpuStart = ClientResources.currentThreadCpuNs();
                while (BenchMetricHeader.nowNs() < until) {
                    long index = submitted.getAndIncrement();
                    long t0 = BenchMetricHeader.nowNs();
                    int depth = inFlight.incrementAndGet();
                    peak.accumulateAndGet(depth, Math::max);
                    try {
                        operation.invoke(payloadSize, phase, index)
                            .get(options.requestTimeoutMs, TimeUnit.MILLISECONDS);
                    } catch (Exception error) {
                        errors.incrementAndGet();
                    } finally {
                        inFlight.decrementAndGet();
                    }
                    if (latencies != null) {
                        latencies.add((BenchMetricHeader.nowNs() - t0) / 1000.0);
                    }
                }
                // Reported before the thread exits: a dead thread's CPU can no
                // longer be read from ThreadMXBean.
                if (resources != null) {
                    resources.addSubmitCpuNs(
                        ClientResources.currentThreadCpuNs() - workerCpuStart);
                }
            }, "bench-send-" + slot);
            worker.start();
            workers.add(worker);
        }
        for (Thread worker : workers) {
            try {
                worker.join();
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
            }
        }
        long total = submitted.get();
        return new SendOutcome(
            new Counts(total - errors.get(), errors.get(), total - errors.get()),
            peak.get());
    }

    // --- warmup ------------------------------------------------------------

    /**
     * spec section 8.2: the JVM reaches steady state only after JIT warmup, so the java row
     * warms up through the same driver the measured window uses and RECORDS the
     * throughput of each warmup segment. Those segments are the evidence that the
     * runtime had stabilized before the measured window opened; they are reported on
     * every cell rather than asserted in prose. The length is one value for the whole
     * run and is never tuned per cell.
     */
    private List<Double> warmup(
        int payloadSize, BenchOperation operation, WarmupSegment segment) {
        List<Double> throughputs = new ArrayList<>();
        double remaining = options.warmupSeconds;
        while (remaining > 0.0) {
            double seconds = Math.min(options.warmupSegmentSeconds, remaining);
            remaining -= seconds;
            Counts counts = segment.run(seconds);
            throughputs.add(counts.completed / Math.max(seconds, 1e-9));
        }
        return throughputs;
    }

    @FunctionalInterface
    private interface WarmupSegment {
        Counts run(double seconds);
    }

    // --- cell assembly -----------------------------------------------------

    private Map<String, Object> requestCell(
        int payloadSize,
        ClientResources.Usage usage,
        StatsClient.ServerSnapshot server,
        Latencies latencies,
        Counts counts,
        int peakInFlight,
        int abandoned,
        long completedAtClose,
        int submitParallelism,
        List<Double> warmupSegments) {
        double seconds = Math.max(1e-9, options.durationSeconds);
        double throughput = counts.completed / seconds;
        Latencies.Summary summary = latencies.summary();

        Map<String, Object> cell = new LinkedHashMap<>();
        cell.put("throughput_per_second", throughput);
        cell.put("bandwidth_mb_s", (throughput * payloadSize) / 1e6);
        cell.put("latency_mean_ms", summary.meanMicros() / 1000.0);
        cell.put("latency_p95_ms", summary.p95Micros() / 1000.0);
        cell.put("latency_p99_ms", summary.p99Micros() / 1000.0);
        putClientColumns(cell, usage);
        cell.put("server_cpu_percent",
            (server.cpuSeconds() / usage.elapsedSeconds() / ClientResources.LOGICAL_CORES)
                * 100.0);
        cell.put("server_memory_mb", server.workingSetMb());
        putDiagnostics(cell, usage, peakInFlight, options.requestWindow, abandoned,
            submitParallelism, warmupSegments);
        cell.put("errors", counts.errors);
        cell.put("completed", counts.completed);
        // Reported so that completions landing in the bounded settle window are
        // visible rather than folded silently into the rate.
        cell.put("completed_at_close", completedAtClose);
        cell.put("latency_samples", summary.count());
        return cell;
    }

    private static void putClientColumns(Map<String, Object> cell, ClientResources.Usage usage) {
        cell.put("client_cpu_percent", usage.cpuPercent());
        cell.put("client_memory_mb", usage.memoryMb());
    }

    private static void putDiagnostics(
        Map<String, Object> cell,
        ClientResources.Usage usage,
        int peakInFlight,
        int window,
        int abandoned,
        int submitParallelism,
        List<Double> warmupSegments) {
        // The declared instrument and its ceiling (spec 5.1, FB-023).
        cell.put("client_saturation_metric", ClientResources.CLIENT_SATURATION_METRIC);
        cell.put("jvm_thread_cores", usage.submitCores());
        cell.put("client_parallelism_ceiling", submitParallelism);
        // Observations. Neither decides saturation.
        cell.put("client_cores", usage.cores());
        cell.put("jvm_all_thread_cores", usage.jvmAllThreadCores());
        cell.put("logical_cores", ClientResources.LOGICAL_CORES);
        cell.put("peak_in_flight", peakInFlight);
        cell.put("request_window", window);
        cell.put("abandoned", abandoned);
        cell.put("warmup_segment_throughput", warmupSegments);
    }

    private record Counts(long completed, long errors, long completedAtClose) {
    }

    private record WindowOutcome(
        Counts counts, int peakInFlight, int abandoned, long completedAtClose) {
    }

    private record SendOutcome(Counts counts, int peakInFlight) {
    }
}
