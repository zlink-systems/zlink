using System.Diagnostics.Metrics;

namespace ZLink.Framework.Perf;

// Standard .NET provider observation, restricted to the documented host capacity instruments.
// It observes existing gauges/counters at collection; it does not synthesize provider metrics.
public sealed class PublicMetricCollector : IDisposable
{
    private readonly MeterListener listener = new();
    private readonly List<object> observations = [];
    private readonly object gate = new();
    public PublicMetricCollector()
    {
        listener.InstrumentPublished = (instrument, owner) =>
        {
            if ((instrument.Name.StartsWith("zlink.host.core_hwm.", StringComparison.Ordinal) ||
                 instrument.Name.StartsWith("zlink.host.application_job_queue.", StringComparison.Ordinal)) &&
                instrument is ObservableGauge<long> or ObservableGauge<double> or ObservableCounter<long> or ObservableCounter<double>)
                owner.EnableMeasurementEvents(instrument);
        };
        listener.SetMeasurementEventCallback<long>((instrument, value, tags, _) => Record(instrument, value, tags));
        listener.SetMeasurementEventCallback<double>((instrument, value, tags, _) =>
        {
            if (!double.IsFinite(value)) throw new InvalidOperationException("Provider returned a non-finite public metric.");
            Record(instrument, value, tags);
        });
        listener.Start();
    }
    private void Record(Instrument instrument, object value, ReadOnlySpan<KeyValuePair<string, object?>> tags)
    {
        var labels = new Dictionary<string, object?>();
        foreach (var tag in tags) labels.Add(tag.Key, tag.Value);
        lock (gate) observations.Add(new { name = instrument.Name,
            kind = instrument is ObservableCounter<long> or ObservableCounter<double> ? "counter" : "observable",
            unit = instrument.Unit, labels, value, meter = instrument.Meter.Name });
    }
    public object[] Snapshot()
    {
        lock (gate)
        {
            observations.Clear();
            listener.RecordObservableInstruments();
            if (observations.Count == 0) throw new InvalidOperationException("No public host capacity metrics were collected.");
            return observations.ToArray();
        }
    }
    public void Dispose() => listener.Dispose();
}
