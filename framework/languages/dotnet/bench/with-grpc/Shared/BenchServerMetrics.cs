using System.Diagnostics;

namespace WithGrpcBench.Shared;

public sealed class BenchServerMetrics
{
    private readonly object _gate = new();
    private readonly List<long> _latencyNs = [];
    private long _activeMessages;
    private long _errors;
    private TimeSpan _cpuStart = Process.GetCurrentProcess().TotalProcessorTime;

    public void Reset()
    {
        lock (_gate)
        {
            _activeMessages = 0;
            _errors = 0;
            _latencyNs.Clear();
            _cpuStart = Process.GetCurrentProcess().TotalProcessorTime;
        }
    }

    public void Record(BenchPayload payload)
    {
        Record(payload.Body.Span);
    }

    public void Record(ReadOnlySpan<byte> payload)
    {
        if (!BenchMetricHeaders.TryDecode(payload, out var header) || header.Phase != BenchPhase.Active)
        {
            return;
        }

        var latency = Math.Max(0, BenchMetricHeaders.NowNs() - header.SentTimestampNs);
        lock (_gate)
        {
            _activeMessages++;
            _latencyNs.Add(latency);
        }
    }

    public void RecordError()
    {
        Interlocked.Increment(ref _errors);
    }

    public BenchServerSnapshot Snapshot()
    {
        lock (_gate)
        {
            var process = Process.GetCurrentProcess();
            var cpuSeconds = Math.Max(0, (process.TotalProcessorTime - _cpuStart).TotalSeconds);
            var samples = _latencyNs.ToArray();
            Array.Sort(samples);
            return new BenchServerSnapshot(
                _activeMessages,
                Interlocked.Read(ref _errors),
                Mean(samples) / 1000.0,
                Percentile(samples, 0.50) / 1000.0,
                Percentile(samples, 0.95) / 1000.0,
                Percentile(samples, 0.99) / 1000.0,
                cpuSeconds,
                process.WorkingSet64 / 1024.0 / 1024.0);
        }
    }

    private static double Mean(long[] sortedSamples)
    {
        if (sortedSamples.Length == 0) return 0;
        return sortedSamples.Average();
    }

    private static long Percentile(long[] sortedSamples, double percentile)
    {
        if (sortedSamples.Length == 0) return 0;
        var index = (int)Math.Ceiling(percentile * sortedSamples.Length) - 1;
        return sortedSamples[Math.Clamp(index, 0, sortedSamples.Length - 1)];
    }
}

public sealed record BenchServerSnapshot(
    long ActiveMessages,
    long Errors,
    double MeanMicros,
    double P50Micros,
    double P95Micros,
    double P99Micros,
    double CpuSeconds,
    double WorkingSetMb)
{
    public static BenchServerSnapshot Empty { get; } = new(0, 0, 0, 0, 0, 0, 0, 0);
}
