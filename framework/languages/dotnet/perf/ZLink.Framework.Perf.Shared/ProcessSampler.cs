using System.Diagnostics;

namespace ZLink.Framework.Perf;

public sealed class ProcessSampler(int capacity) : IDisposable
{
    private readonly Process process = Process.GetCurrentProcess();
    private readonly List<long> sampleIntervals = new(capacity);
    private long started, lastSample, allocatedStart, allocatedEnd, rssMax;
    private TimeSpan cpuStart, cpuEnd, cpuLast;
    private long windowStart, windowEnd;
    private readonly List<CpuSample> cpuSamples = new(capacity);
    private sealed record CpuSample(int binIndex, double startOffsetMs, double endOffsetMs, string observedDurationNs, string cpuDeltaNs);
    private int[] gcStart = [], gcEnd = [];
    public void Start(long start, long end)
    {
        windowStart = start; windowEnd = end;
        process.Refresh();
        cpuStart = cpuLast = process.TotalProcessorTime;
        started = lastSample = PerfClock.Now;
        cpuSamples.Clear();
        allocatedStart = GC.GetTotalAllocatedBytes(false);
        gcStart = Enumerable.Range(0, 3).Select(GC.CollectionCount).ToArray();
        rssMax = process.WorkingSet64;
        sampleIntervals.Clear();
    }
    public void Sample()
    {
        process.Refresh();
        var cpuNow = process.TotalProcessorTime;
        var now = PerfClock.Now;
        var span = now - lastSample;
        var delta = checked((cpuNow - cpuLast).Ticks * 100L);
        if (span <= 0 || delta < 0) throw new PerfValidationException("CpuSampleInvalid", "CPU sampling requires positive elapsed time and nonnegative cumulative delta.");
        if (lastSample >= windowStart && lastSample < windowEnd) cpuSamples.Add(new(checked((int)((lastSample - windowStart) / 100_000_000L)), (lastSample - windowStart) / 1e6, (now - windowStart) / 1e6, DecimalText.Of(span), DecimalText.Of(delta)));
        sampleIntervals.Add(span);
        lastSample = now; cpuLast = cpuNow;
        rssMax = Math.Max(rssMax, process.WorkingSet64);
    }
    public void End()
    {
        Sample();
        cpuEnd = cpuLast;
        allocatedEnd = GC.GetTotalAllocatedBytes(false);
        gcEnd = Enumerable.Range(0, 3).Select(GC.CollectionCount).ToArray();
    }
    public double? BinCpu(int index)
    {
        var samples = cpuSamples.Where(s => s.binIndex == index).ToArray();
        return samples.Length == 0 ? null : 100.0 * samples.Sum(s => double.Parse(s.cpuDeltaNs, System.Globalization.CultureInfo.InvariantCulture)) / samples.Sum(s => double.Parse(s.observedDurationNs, System.Globalization.CultureInfo.InvariantCulture));
    }
    public void Export(Dictionary<string, object?> metrics, Dictionary<string, object?> runtime)
    {
        var seconds = (lastSample - started) / 1e9;
        metrics["process.cpuPercent"] = seconds > 0 ? (cpuEnd - cpuStart).TotalSeconds / seconds * 100 : 0;
        metrics["process.rssMb"] = rssMax / 1048576.0;
        metrics["process.allocatedMb"] = Math.Max(0, allocatedEnd - allocatedStart) / 1048576.0;
        for (var i = 0; i < 3; i++) metrics["gc.gen" + i] = DecimalText.Of((ulong)Math.Max(0, gcEnd[i] - gcStart[i]));
        runtime["cpuSamples"] = new { name = "Process.TotalProcessorTime actual spans, assigned by span start", unit = "observation", type = "array", value = cpuSamples.ToArray() };
        runtime["rssSampling"] = new { name = "Process.WorkingSet64", unit = "ns", type = "sampling",
            value = new { requestedIntervalNs = "100000000", actualIntervalsNs = sampleIntervals.Select(DecimalText.Of).ToArray(),
                startedTicks = DecimalText.Of(started), endedTicks = DecimalText.Of(lastSample) } };
        runtime["cpuObservationSeconds"] = new { name = "Process.TotalProcessorTime observation span", unit = "s", type = "number", value = seconds };
    }
    public void Dispose() => process.Dispose();
}
