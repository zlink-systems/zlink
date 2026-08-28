/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;



import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.LongAdder;

final class PerfMetricsCollector {
    private final boolean singleThreaded;
    private final LongAdder count = new LongAdder();
    private final LongAdder sampleCount = new LongAdder();
    private final LongAdder sum = new LongAdder();
    private long singleCount;
    private long singleSampleCount;
    private long singleSum;
    private ThreadReservoir singleReservoir;
    // Per-thread latency reservoirs use the same bounded sampling rule as the
    // C harness. The cap bounds measurement memory and must not turn an
    // unbounded Java array into part of the measured receive path.
    private final ThreadLocal<ThreadReservoir> threadReservoir;
    private final List<ThreadReservoir> registry =
        new ArrayList<>();
    private final Object registryLock = new Object();

    PerfMetricsCollector(String suite) {
        singleThreaded = "single".equals(suite);
        threadReservoir = ThreadLocal.withInitial(() -> {
            ThreadReservoir reservoir = new ThreadReservoir(
                resolveInitialLatencyCapacity(suite));
            synchronized (registryLock) {
                registry.add(reservoir);
            }
            return reservoir;
        });
    }

    void recordNanos(long value) {
        if (singleThreaded) {
            singleCount++;
            singleSampleCount++;
            singleSum += value;
            singleReservoir().add(value);
            return;
        }
        count.increment();
        sampleCount.increment();
        sum.add(value);
        ThreadReservoir reservoir = threadReservoir.get();
        reservoir.add(value);
    }

    void recordEvent() {
        if (singleThreaded) {
            singleCount++;
            return;
        }
        count.increment();
    }

    void recordLatencySampleNanos(long value) {
        if (singleThreaded) {
            singleSampleCount++;
            singleSum += value;
            singleReservoir().add(value);
            return;
        }
        sampleCount.increment();
        sum.add(value);
        ThreadReservoir reservoir = threadReservoir.get();
        reservoir.add(value);
    }

    PerfUtil.Result finishSingle(PerfUtil.Config config) {
        return finish(config, 1_000_000.0d);
    }

    PerfUtil.Result finishMulti(PerfUtil.Config config) {
        return finish(config, 1_000_000.0d);
    }

    private PerfUtil.Result finish(PerfUtil.Config config, double latencyDivisor) {
        long totalCount = singleThreaded ? singleCount : count.sum();
        if (totalCount == 0) {
            return new PerfUtil.Result("fail", "no_active_samples", config.pattern(),
                config.transport(), config.size(), 0.0d, 0.0d, 0.0d,
                0.0d, 0.0d);
        }
        // Merge per-thread reservoirs.
        List<ThreadReservoir> snapshot;
        if (singleThreaded) {
            snapshot = singleReservoir == null
                ? List.of()
                : List.of(singleReservoir);
        } else {
            synchronized (registryLock) {
                snapshot = new ArrayList<>(registry);
            }
        }
        List<long[]> samples = new ArrayList<>(snapshot.size());
        int totalLen = 0;
        for (ThreadReservoir reservoir : snapshot) {
            long[] reservoirSamples = reservoir.snapshot();
            samples.add(reservoirSamples);
            totalLen += reservoirSamples.length;
        }
        long[] merged = new long[totalLen];
        int offset = 0;
        for (long[] reservoirSamples : samples) {
            System.arraycopy(reservoirSamples, 0, merged, offset,
                reservoirSamples.length);
            offset += reservoirSamples.length;
        }
        Arrays.sort(merged);
        double throughput = totalCount / (double) config.durationSeconds();
        double bandwidth = throughput * config.size()
            * (PerfMeasurement.isEchoPattern(config.suite(), config.pattern())
                ? 2.0d : 1.0d)
            / 1_000_000.0d;
        long totalSampleCount = singleThreaded
            ? singleSampleCount
            : sampleCount.sum();
        long totalSum = singleThreaded ? singleSum : sum.sum();
        double mean = totalSampleCount == 0
            ? 0.0d
            : (totalSum / (double) totalSampleCount) / latencyDivisor;
        double p95 = percentile(merged, 0.95d) / latencyDivisor;
        double p99 = percentile(merged, 0.99d) / latencyDivisor;
        return new PerfUtil.Result("ok", "-", config.pattern(), config.transport(),
            config.size(), throughput, bandwidth, mean, p95, p99);
    }

    private ThreadReservoir singleReservoir() {
        if (singleReservoir == null) {
            singleReservoir = new ThreadReservoir(
                resolveInitialLatencyCapacity("single"));
        }
        return singleReservoir;
    }

    // Per-thread sample storage; no synchronization required because each
    // reservoir is owned by exactly one thread for its hot path lifetime.
    private static final class ThreadReservoir {
        private long[] samples;
        private int size;
        private final int sampleCap;
        private long samplesSeen;
        private int randomState = 0xA341316C;

        ThreadReservoir(int sampleCap) {
            this.sampleCap = Math.max(0, sampleCap);
            this.samples = new long[this.sampleCap];
        }

        void add(long value) {
            samplesSeen++;
            if (sampleCap == 0) {
                return;
            }
            if (size < sampleCap) {
                samples[size++] = value;
                return;
            }
            randomState = randomState * 1664525 + 1013904223;
            long slot = Integer.toUnsignedLong(randomState) % samplesSeen;
            if (slot < sampleCap) {
                samples[(int) slot] = value;
            }
        }

        long[] snapshot() {
            return Arrays.copyOf(samples, size);
        }
    }

    private static double percentile(long[] sorted, double percentile) {
        if (sorted.length == 0) {
            return 0.0d;
        }
        if (percentile <= 0.0d) {
            return sorted[0];
        }
        if (percentile >= 1.0d) {
            return sorted[sorted.length - 1];
        }
        double pos = (sorted.length - 1) * percentile;
        int lo = (int) pos;
        int hi = lo + 1 < sorted.length ? lo + 1 : lo;
        double frac = pos - lo;
        return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
    }

    private static int resolveInitialLatencyCapacity(String suite) {
        String envName = "multi".equals(suite)
            ? "PERF_MULTI_LATENCY_SAMPLE_CAP"
            : "PERF_SINGLE_LATENCY_SAMPLE_CAP";
        int fallback = "multi".equals(suite) ? 65_536 : 200_000;
        return Math.max(0, intEnv(envName, fallback));
    }

    private static int intEnv(String name, int fallback) {
        String value = System.getenv(name);
        if (value == null || value.isBlank()) {
            return fallback;
        }
        return Integer.parseInt(value);
    }
}
