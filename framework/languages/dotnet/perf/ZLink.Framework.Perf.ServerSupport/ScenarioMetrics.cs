namespace ZLink.Framework.Perf;

// The §14 family keys a scenario fills beside the shared echo counters (driver.*, spot.*, worker.*, messages.admitted...).
// Counters and histograms clear with the window at reset and are written into every snapshot; a key the scenario
// declares as unsupported keeps its null with the public-observation reason. Keys it does not name stay as
// Measurement wrote them (null, NOT_APPLICABLE).
public sealed class ScenarioMetrics
{
    private readonly object gate = new();
    private readonly Dictionary<string, ulong> counts = [];
    private readonly Dictionary<string, (string Prefix, Histogram Histogram)> histograms = [];
    private readonly List<Action> resets = [];
    private readonly List<(string From, string To)> aliases = [];
    private readonly Dictionary<string, (string Code, string Reason)> unsupported = [];
    private readonly Dictionary<string, object?> provenance = [];
    private readonly Measurement measurement;

    public ScenarioMetrics(Measurement measurement)
    {
        this.measurement = measurement;
        measurement.OnReset = Reset;
        measurement.EnrichSnapshot = Enrich;
    }

    // Counter keys the scenario measures; they read "0" until observed instead of staying null.
    public ScenarioMetrics Counters(params string[] keys)
    {
        lock (gate) foreach (var key in keys) counts.TryAdd(key, 0);
        return this;
    }
    // A latency histogram (§15.3): the histogram key and the dotted metric prefix it exports.
    public ScenarioMetrics Latency(string histogramKey, string prefix)
    {
        lock (gate) histograms[histogramKey] = (prefix, new Histogram());
        return this;
    }
    public ScenarioMetrics Unsupported(string code, string reason, params string[] keys)
    {
        lock (gate) foreach (var key in keys) unsupported[key] = (code, reason);
        return this;
    }
    // §14.1: no public observation of a Spot's mailbox or turn internals; a Spot workload keeps these null with that reason.
    public ScenarioMetrics SpotInternalsUnsupported() => Unsupported("PUBLIC_OBSERVATION_UNSUPPORTED",
        "Public status is a host aggregate; no per-Spot mailbox, turn or resume observation exists.",
        "spot.mailboxDepth.max", "spot.mailboxDepth.mean", "spot.suspendedTurns", "spot.resumedTurns",
        "spot.resumeLatency.p95Ms", "spot.resumeLatency.p99Ms");
    // A metric family that is the same interval as another (§15.3: spot.remoteCallLatency.* is latency.*).
    public ScenarioMetrics AliasLatency(string fromPrefix, string toPrefix)
    {
        lock (gate) aliases.Add((fromPrefix, toPrefix));
        return this;
    }
    public ScenarioMetrics Provenance(string key, object? value)
    {
        lock (gate) provenance[key] = value;
        return this;
    }
    // State a scenario keeps beside the counters (a correlation table) clears with the same reset.
    public void OnReset(Action reset) { lock (gate) resets.Add(reset); }

    public void Count(string key, ulong amount = 1)
    {
        lock (gate) counts[key] = checked(counts.GetValueOrDefault(key) + amount);
    }
    // Only a sample whose operation finished inside the measured window belongs to the window histogram.
    public void Record(string histogramKey, long startedTicks, long completedTicks) =>
        Record(histogramKey, startedTicks, completedTicks, completedTicks);
    // windowTicks: when the operation this interval belongs to finished (a worker interval ends before its operation does).
    public void Record(string histogramKey, long startedTicks, long endedTicks, long windowTicks)
    {
        if (windowTicks >= measurement.EndTicks) return;
        lock (gate) histograms[histogramKey].Histogram.Record(endedTicks - startedTicks);
    }

    private void Reset()
    {
        lock (gate)
        {
            foreach (var key in counts.Keys.ToArray()) counts[key] = 0;
            foreach (var key in histograms.Keys.ToArray()) histograms[key] = (histograms[key].Prefix, new Histogram());
            foreach (var reset in resets) reset();
        }
    }

    private void Enrich(PerfMetricsSnapshot snapshot)
    {
        lock (gate)
        {
            foreach (var (key, value) in counts)
            {
                snapshot.metrics[key] = DecimalText.Of(value);
                snapshot.nullReasons.Remove("/metrics/" + key);
            }
            foreach (var (key, (prefix, histogram)) in histograms)
            {
                foreach (var suffix in MetricCatalog.LatencySuffixes) snapshot.nullReasons.Remove("/metrics/" + prefix + "." + suffix);
                snapshot.nullReasons.Remove("/histograms/" + key);
                histogram.Export(key, prefix, snapshot.metrics, snapshot.histograms, snapshot.nullReasons);
            }
            foreach (var (from, to) in aliases)
                foreach (var suffix in MetricCatalog.LatencySuffixes)
                {
                    snapshot.metrics[to + "." + suffix] = snapshot.metrics[from + "." + suffix];
                    if (snapshot.nullReasons.TryGetValue("/metrics/" + from + "." + suffix, out var reason)) snapshot.nullReasons["/metrics/" + to + "." + suffix] = reason;
                    else snapshot.nullReasons.Remove("/metrics/" + to + "." + suffix);
                }
            foreach (var (key, (code, reason)) in unsupported)
            {
                snapshot.metrics[key] = null;
                snapshot.nullReasons["/metrics/" + key] = new(code, reason, "spec/server/06-observability/01-runtime-monitoring");
            }
            foreach (var (key, value) in provenance) snapshot.provenance[key] = value;
        }
    }
}
