/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Latency samples in microseconds. Completions arrive on binding and gRPC threads,
 * so the store is a preallocated array with an atomic cursor: no lock is taken on
 * the completion path, and a sample past the limit is dropped rather than growing
 * the array inside a measured window.
 */
public final class Latencies {
    private final double[] samples;
    private final AtomicInteger cursor = new AtomicInteger();

    public Latencies(int limit) {
        this.samples = new double[Math.max(limit, 1)];
    }

    public void add(double micros) {
        int index = cursor.getAndIncrement();
        if (index < samples.length) {
            samples[index] = micros;
        }
    }

    public Summary summary() {
        int count = Math.min(cursor.get(), samples.length);
        double[] sorted = Arrays.copyOf(samples, count);
        Arrays.sort(sorted);
        return new Summary(
            mean(sorted), percentile(sorted, 0.95), percentile(sorted, 0.99), count);
    }

    private static double mean(double[] sorted) {
        if (sorted.length == 0) {
            return 0;
        }
        double total = 0;
        for (double value : sorted) {
            total += value;
        }
        return total / sorted.length;
    }

    private static double percentile(double[] sorted, double fraction) {
        if (sorted.length == 0) {
            return 0;
        }
        int index = (int) Math.ceil(fraction * sorted.length) - 1;
        return sorted[Math.min(Math.max(index, 0), sorted.length - 1)];
    }

    public record Summary(double meanMicros, double p95Micros, double p99Micros, int count) {
    }
}
