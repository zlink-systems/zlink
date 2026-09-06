/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import java.lang.management.ManagementFactory;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

/**
 * What a bench server counts.
 *
 * <p>spec section 5 / G3: {@code send-saturation} throughput is the number of messages
 * the SERVER received during the active phase, never the client's submit count.
 * That is why every server carries this and why the counter only advances for
 * payloads whose header says phase == active.
 */
public final class BenchServerMetrics {
    private static final int LATENCY_LIMIT = 500_000;

    private final AtomicLong activeMessages = new AtomicLong();
    // Every payload the handler saw, whatever its phase. It separates "the server
    // never received this" from "the server received it but the header did not say
    // active", which are two very different explanations for a throughput of zero.
    private final AtomicLong totalMessages = new AtomicLong();
    private final AtomicLong errors = new AtomicLong();
    private final Object latencyLock = new Object();
    private List<Long> latencyNs = new ArrayList<>();
    private volatile long cpuStartNs = processCpuNs();
    private volatile long wallStartNs = BenchMetricHeader.nowNs();

    public void reset() {
        activeMessages.set(0);
        totalMessages.set(0);
        errors.set(0);
        synchronized (latencyLock) {
            latencyNs = new ArrayList<>();
        }
        cpuStartNs = processCpuNs();
        wallStartNs = BenchMetricHeader.nowNs();
    }

    /** {@code body} is the stamped payload body, not the protobuf envelope. */
    public void record(ByteBuffer body) {
        totalMessages.incrementAndGet();
        BenchMetricHeader.Decoded decoded = BenchMetricHeader.decode(body);
        if (decoded == null || decoded.phase() != BenchMetricHeader.PHASE_ACTIVE) {
            return;
        }
        activeMessages.incrementAndGet();
        long latency = BenchMetricHeader.nowNs() - decoded.sentTimestampNs();
        synchronized (latencyLock) {
            if (latencyNs.size() < LATENCY_LIMIT) {
                latencyNs.add(Math.max(latency, 0L));
            }
        }
    }

    public void recordError() {
        errors.incrementAndGet();
    }

    public Snapshot snapshot() {
        List<Long> samples;
        synchronized (latencyLock) {
            samples = new ArrayList<>(latencyNs);
        }
        Collections.sort(samples);
        Runtime runtime = Runtime.getRuntime();
        double cpuSeconds = (processCpuNs() - cpuStartNs) / 1e9;
        double elapsedSeconds = (BenchMetricHeader.nowNs() - wallStartNs) / 1e9;
        return new Snapshot(
            activeMessages.get(),
            totalMessages.get(),
            errors.get(),
            mean(samples) / 1000.0,
            percentile(samples, 0.50) / 1000.0,
            percentile(samples, 0.95) / 1000.0,
            percentile(samples, 0.99) / 1000.0,
            cpuSeconds,
            elapsedSeconds,
            (runtime.totalMemory() - runtime.freeMemory()) / 1024.0 / 1024.0,
            residentSetMb());
    }

    private static double mean(List<Long> sorted) {
        if (sorted.isEmpty()) {
            return 0;
        }
        double total = 0;
        for (long value : sorted) {
            total += value;
        }
        return total / sorted.size();
    }

    private static double percentile(List<Long> sorted, double fraction) {
        if (sorted.isEmpty()) {
            return 0;
        }
        int index = (int) Math.ceil(fraction * sorted.size()) - 1;
        return sorted.get(Math.min(Math.max(index, 0), sorted.size() - 1));
    }

    /** Process CPU nanoseconds, or -1 when the platform bean is unavailable. */
    public static long processCpuNs() {
        java.lang.management.OperatingSystemMXBean bean =
            ManagementFactory.getOperatingSystemMXBean();
        if (bean instanceof com.sun.management.OperatingSystemMXBean sun) {
            return sun.getProcessCpuTime();
        }
        return -1;
    }

    /** Resident set in megabytes, read from /proc/self/statm. */
    public static double residentSetMb() {
        try {
            String line = java.nio.file.Files.readString(
                java.nio.file.Path.of("/proc/self/statm")).trim();
            String[] fields = line.split("\\s+");
            long pages = Long.parseLong(fields[1]);
            return pages * 4096.0 / 1024.0 / 1024.0;
        } catch (Exception error) {
            Runtime runtime = Runtime.getRuntime();
            return (runtime.totalMemory() - runtime.freeMemory()) / 1024.0 / 1024.0;
        }
    }

    public record Snapshot(
        long activeMessages,
        long totalMessages,
        long errors,
        double meanMicros,
        double p50Micros,
        double p95Micros,
        double p99Micros,
        double cpuSeconds,
        double elapsedSeconds,
        double heapMb,
        double workingSetMb) {

        public String toJson() {
            return "{\"activeMessages\":" + activeMessages
                + ",\"totalMessages\":" + totalMessages
                + ",\"errors\":" + errors
                + ",\"meanMicros\":" + meanMicros
                + ",\"p50Micros\":" + p50Micros
                + ",\"p95Micros\":" + p95Micros
                + ",\"p99Micros\":" + p99Micros
                + ",\"cpuSeconds\":" + cpuSeconds
                + ",\"elapsedSeconds\":" + elapsedSeconds
                + ",\"heapMb\":" + heapMb
                + ",\"workingSetMb\":" + workingSetMb + "}";
        }
    }
}
