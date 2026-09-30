package systems.zlink.framework.perf;

import com.fasterxml.jackson.databind.ObjectMapper;

import java.io.IOException;
import java.io.InputStream;
import java.math.BigInteger;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// §15.3 application latency histogram. The bounds are framework/perf/schema/histogram-bounds.json (packaged as a
// resource of this project); the runner aggregates the originals, so only count/sum/max and the bucket counts are kept.
// Not thread-safe: the owner (Measurement, ScenarioMetrics) records under its own lock.
public final class Histogram {
    public static final double[] BOUNDS = loadBounds();
    private static final long[] BOUNDS_NS = boundsNs();

    private final long[] counts = new long[BOUNDS.length];
    private long count;
    private long overflow;
    private long max;
    private long sum;
    private BigInteger bigSum;

    private static double[] loadBounds() {
        try (InputStream stream = Histogram.class.getResourceAsStream("/histogram-bounds.json")) {
            if (stream == null) {
                throw new IllegalStateException("histogram-bounds.json is not packaged");
            }
            return new ObjectMapper().readValue(stream, double[].class);
        } catch (IOException error) {
            throw new IllegalStateException("histogram-bounds.json is unreadable", error);
        }
    }

    private static long[] boundsNs() {
        long[] result = new long[BOUNDS.length];
        for (int i = 0; i < result.length; i++) {
            result[i] = Math.round(BOUNDS[i] * 1_000_000);
        }
        return result;
    }

    public void record(long elapsedNs) {
        if (elapsedNs < 0) {
            throw new IllegalArgumentException("elapsedNs");
        }
        count = Math.addExact(count, 1);
        int bucket = 0;
        while (bucket < BOUNDS_NS.length && elapsedNs > BOUNDS_NS[bucket]) {
            bucket++;
        }
        if (bucket == BOUNDS_NS.length) {
            overflow++;
        } else {
            counts[bucket]++;
        }
        if (bigSum == null) {
            long next = sum + elapsedNs;
            if (((sum ^ next) & (elapsedNs ^ next)) < 0) {
                bigSum = BigInteger.valueOf(sum).add(BigInteger.valueOf(elapsedNs));
            } else {
                sum = next;
            }
        } else {
            bigSum = bigSum.add(BigInteger.valueOf(elapsedNs));
        }
        max = Math.max(max, elapsedNs);
    }

    private String sumText() {
        return bigSum == null ? Long.toString(sum) : bigSum.toString();
    }

    private double sumDouble() {
        return bigSum == null ? (double) sum : bigSum.doubleValue();
    }

    /** The §15.3 histogram object. */
    public Map<String, Object> snapshot() {
        Map<String, Object> value = new LinkedHashMap<>();
        value.put("unit", "ms");
        value.put("ticksUnit", "ns");
        List<Double> bounds = new ArrayList<>();
        for (double bound : BOUNDS) {
            bounds.add(bound);
        }
        value.put("bounds", bounds);
        List<String> texts = new ArrayList<>();
        for (long bucket : counts) {
            texts.add(Long.toString(bucket));
        }
        value.put("counts", texts);
        value.put("overflow", Long.toString(overflow));
        value.put("count", Long.toString(count));
        value.put("sumNs", sumText());
        value.put("maxNs", count == 0 ? null : Long.toString(max));
        value.put("percentileMethod", "nearest-rank-bucket-upper-bound");
        return value;
    }

    /** Writes the histogram and its dotted `<prefix>.{meanMs,p50Ms,p95Ms,p99Ms,maxMs}` metrics (§14, §15.3). */
    public void export(String histogramKey, String metricPrefix, Map<String, Object> metrics,
            Map<String, Object> histograms, Map<String, NullReason> reasons) {
        histograms.put(histogramKey, snapshot());
        for (String name : MetricCatalog.LATENCY_SUFFIXES) {
            String key = metricPrefix + "." + name;
            Double value = null;
            if (count != 0) {
                value = switch (name) {
                    case "meanMs" -> sumDouble() / count / 1_000_000;
                    case "maxMs" -> max / 1_000_000.0;
                    default -> percentile(Integer.parseInt(name.substring(1, 3)));
                };
            }
            metrics.put(key, value);
            if (value == null) {
                reasons.put("/metrics/" + key, count == 0
                        ? new NullReason("NO_SAMPLES", "No successful samples in this cohort and window.")
                        : new NullReason("HISTOGRAM_OVERFLOW", "Nearest rank lies above the final bucket.",
                                "perf/README.ko.md", 1024.0));
            }
        }
        if (count == 0) {
            reasons.put("/histograms/" + histogramKey + "/maxNs",
                    new NullReason("NO_SAMPLES", "No successful samples in this cohort and window."));
        }
    }

    private Double percentile(int percentile) {
        long rank = (percentile * count + 99) / 100;
        long cumulative = 0;
        for (int i = 0; i < counts.length; i++) {
            cumulative += counts[i];
            if (cumulative >= rank) {
                return BOUNDS[i];
            }
        }
        return null;
    }
}
