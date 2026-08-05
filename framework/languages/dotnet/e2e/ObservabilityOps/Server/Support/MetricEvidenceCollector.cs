using System.Diagnostics.Metrics;
using ObservabilityOps.Shared;
using Zlink.Framework.E2E.Diagnostics;

namespace ObservabilityOps.Server.Support;

public sealed class MetricEvidenceCollector : IDisposable
{
    private readonly object _gate = new();
    private readonly MeterListener _listener = new();
    private readonly SemaphoreSlim _changed = new(0);
    private readonly Dictionary<string, Series> _events = new(StringComparer.Ordinal);
    private Dictionary<string, Series> _observables = new(StringComparer.Ordinal);
    private Dictionary<string, Series>? _activeScrape;

    public MetricEvidenceCollector()
    {
        _listener.InstrumentPublished = static (instrument, listener) =>
        {
            if (instrument.Meter.Name == E2eMessageFlowListener.MeterName)
                listener.EnableMeasurementEvents(instrument);
        };
        _listener.SetMeasurementEventCallback<long>(Record);
        _listener.SetMeasurementEventCallback<double>(Record);
        _listener.Start();
    }

    public MetricSample[] Snapshot()
    {
        lock (_gate) _activeScrape = new Dictionary<string, Series>(StringComparer.Ordinal);
        try { _listener.RecordObservableInstruments(); }
        finally { lock (_gate) { _observables = _activeScrape!; _activeScrape = null; } }
        lock (_gate) return _events.Values.Concat(_observables.Values)
            .Select(series => series.Snapshot()).OrderBy(sample => sample.Name, StringComparer.Ordinal).ToArray();
    }

    public async Task<MetricSample[]> WaitAsync(
        Func<MetricSample[], bool> predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (true)
        {
            var snapshot = Snapshot();
            if (predicate(snapshot)) return snapshot;
            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero) throw new TimeoutException("Metric evidence wait timed out.");
            await _changed.WaitAsync(remaining, cancellationToken);
        }
    }

    private void Record<T>(Instrument instrument, T value,
        ReadOnlySpan<KeyValuePair<string, object?>> tags, object? state) where T : struct
    {
        _ = state;
        var attributes = tags.ToArray().ToDictionary(pair => pair.Key,
            pair => pair.Value?.ToString() ?? string.Empty, StringComparer.Ordinal);
        var kind = Kind(instrument);
        var key = instrument.Name + "|" + kind + "|" + string.Join(",",
            attributes.OrderBy(pair => pair.Key).Select(pair => $"{pair.Key}={pair.Value}"));
        lock (_gate)
        {
            var target = kind == "observable" ? _activeScrape : _events;
            if (target is null) return;
            if (!target.TryGetValue(key, out var series))
                target[key] = series = new Series(instrument.Name, kind, instrument.Unit, attributes);
            series.Record(Convert.ToDecimal(value, System.Globalization.CultureInfo.InvariantCulture));
        }
        _changed.Release();
    }

    private static string Kind(Instrument instrument) => instrument switch
    {
        ObservableGauge<long> or ObservableGauge<double> => "observable",
        UpDownCounter<long> or UpDownCounter<double> => "updown",
        Histogram<long> or Histogram<double> => "histogram",
        _ => "counter"
    };

    public void Dispose() => _listener.Dispose();

    private sealed class Series(string name, string kind, string? unit,
        IReadOnlyDictionary<string, string> tags)
    {
        private decimal _value, _sum;
        private decimal? _min, _max;
        private long _count;
        public void Record(decimal value)
        {
            _value = kind == "histogram" ? value : _value + value;
            if (kind == "histogram")
            {
                _sum += value; _min = _min is null ? value : Math.Min(_min.Value, value);
                _max = _max is null ? value : Math.Max(_max.Value, value);
            }
            _count++;
        }
        public MetricSample Snapshot() => new(name, kind, _value, unit, tags, _count,
            kind == "histogram" ? _sum : null, _min, _max);
    }
}
