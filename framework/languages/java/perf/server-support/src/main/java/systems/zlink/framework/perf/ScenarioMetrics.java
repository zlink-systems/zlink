package systems.zlink.framework.perf;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// The §14 family keys a scenario fills beside the shared echo counters (driver.*, spot.*, worker.*, messages.admitted...).
// Counters and histograms clear with the window at reset and are written into every snapshot; a key the scenario
// declares as unsupported keeps its null with the public-observation reason. Keys it does not name stay as
// Measurement wrote them (null, NOT_APPLICABLE).
public final class ScenarioMetrics {
    private record Latency(String prefix, Histogram histogram) {}

    private final Object gate = new Object();
    private final Map<String, Long> counts = new LinkedHashMap<>();
    private final Map<String, Latency> histograms = new LinkedHashMap<>();
    private final List<Runnable> resets = new ArrayList<>();
    private final List<String[]> aliases = new ArrayList<>();
    private final Map<String, String[]> unsupported = new LinkedHashMap<>();
    private final Map<String, Object> provenance = new LinkedHashMap<>();
    private final Measurement measurement;

    public ScenarioMetrics(Measurement measurement) {
        this.measurement = measurement;
        measurement.onReset(this::reset);
        measurement.enrichSnapshot(this::enrich);
    }

    /** Counter keys the scenario measures; they read "0" until observed instead of staying null. */
    public ScenarioMetrics counters(String... keys) {
        synchronized (gate) {
            for (String key : keys) {
                counts.putIfAbsent(key, 0L);
            }
        }
        return this;
    }

    /** A latency histogram (§15.3): the histogram key and the dotted metric prefix it exports. */
    public ScenarioMetrics latency(String histogramKey, String prefix) {
        synchronized (gate) {
            histograms.put(histogramKey, new Latency(prefix, new Histogram()));
        }
        return this;
    }

    public ScenarioMetrics unsupported(String code, String reason, String... keys) {
        synchronized (gate) {
            for (String key : keys) {
                unsupported.put(key, new String[] {code, reason});
            }
        }
        return this;
    }

    /** §14.1: no public observation of a Spot's mailbox or turn internals; a Spot workload keeps these null with that reason. */
    public ScenarioMetrics spotInternalsUnsupported() {
        return unsupported("PUBLIC_OBSERVATION_UNSUPPORTED",
                "Public status is a host aggregate; no per-Spot mailbox, turn or resume observation exists.",
                "spot.mailboxDepth.max", "spot.mailboxDepth.mean", "spot.suspendedTurns", "spot.resumedTurns",
                "spot.resumeLatency.p95Ms", "spot.resumeLatency.p99Ms");
    }

    /** A metric family that is the same interval as another (§15.3: spot.remoteCallLatency.* is latency.*). */
    public ScenarioMetrics aliasLatency(String fromPrefix, String toPrefix) {
        synchronized (gate) {
            aliases.add(new String[] {fromPrefix, toPrefix});
        }
        return this;
    }

    public ScenarioMetrics provenance(String key, Object value) {
        synchronized (gate) {
            provenance.put(key, value);
        }
        return this;
    }

    /** State a scenario keeps beside the counters (a correlation table) clears with the same reset. */
    public void onReset(Runnable reset) {
        synchronized (gate) {
            resets.add(reset);
        }
    }

    public void count(String key) {
        count(key, 1);
    }

    public void count(String key, long amount) {
        synchronized (gate) {
            counts.merge(key, amount, (a, b) -> Math.addExact(a, b));
        }
    }

    /** Only a sample whose operation finished inside the measured window belongs to the window histogram. */
    public void record(String histogramKey, long startedTicks, long completedTicks) {
        record(histogramKey, startedTicks, completedTicks, completedTicks);
    }

    /** windowTicks: when the operation this interval belongs to finished (a worker interval ends before its operation does). */
    public void record(String histogramKey, long startedTicks, long endedTicks, long windowTicks) {
        if (windowTicks >= measurement.endTicks()) {
            return;
        }
        synchronized (gate) {
            histograms.get(histogramKey).histogram().record(endedTicks - startedTicks);
        }
    }

    private void reset() {
        synchronized (gate) {
            counts.replaceAll((key, value) -> 0L);
            histograms.replaceAll((key, value) -> new Latency(value.prefix(), new Histogram()));
            for (Runnable reset : resets) {
                reset.run();
            }
        }
    }

    private void enrich(PerfSnapshot snapshot) {
        synchronized (gate) {
            counts.forEach((key, value) -> {
                snapshot.metrics.put(key, DecimalText.of(value));
                snapshot.nullReasons.remove("/metrics/" + key);
            });
            histograms.forEach((key, latency) -> {
                for (String suffix : MetricCatalog.LATENCY_SUFFIXES) {
                    snapshot.nullReasons.remove("/metrics/" + latency.prefix() + "." + suffix);
                }
                snapshot.nullReasons.remove("/histograms/" + key);
                latency.histogram().export(key, latency.prefix(), snapshot.metrics, snapshot.histograms, snapshot.nullReasons);
            });
            for (String[] alias : aliases) {
                for (String suffix : MetricCatalog.LATENCY_SUFFIXES) {
                    snapshot.metrics.put(alias[1] + "." + suffix, snapshot.metrics.get(alias[0] + "." + suffix));
                    NullReason reason = snapshot.nullReasons.get("/metrics/" + alias[0] + "." + suffix);
                    if (reason != null) {
                        snapshot.nullReasons.put("/metrics/" + alias[1] + "." + suffix, reason);
                    } else {
                        snapshot.nullReasons.remove("/metrics/" + alias[1] + "." + suffix);
                    }
                }
            }
            unsupported.forEach((key, reason) -> {
                snapshot.metrics.put(key, null);
                snapshot.nullReasons.put("/metrics/" + key,
                        new NullReason(reason[0], reason[1], "spec/server/06-observability/01-runtime-monitoring"));
            });
            snapshot.provenance.putAll(provenance);
        }
    }
}
