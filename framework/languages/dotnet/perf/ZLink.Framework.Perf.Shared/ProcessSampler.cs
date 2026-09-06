using System.Diagnostics;

namespace ZLink.Framework.Perf;

public sealed class ProcessSampler : IDisposable
{
    private readonly Process process = Process.GetCurrentProcess();
    private readonly List<long> sampleIntervals = [];
    private long started, lastSample, allocatedStart, allocatedEnd, rssMax;
    private TimeSpan cpuStart, cpuEnd;
    private int[] gcStart = [], gcEnd = [];
    public void Start()
    {
        started = lastSample = PerfClock.Now;
        process.Refresh();
        cpuStart = process.TotalProcessorTime;
        allocatedStart = GC.GetTotalAllocatedBytes(false);
        gcStart = Enumerable.Range(0, 3).Select(GC.CollectionCount).ToArray();
        rssMax = process.WorkingSet64;
        sampleIntervals.Clear();
    }
    public void Sample()
    {
        var now = PerfClock.Now;
        sampleIntervals.Add(now - lastSample);
        lastSample = now;
        process.Refresh();
        rssMax = Math.Max(rssMax, process.WorkingSet64);
    }
    public void End()
    {
        Sample();
        cpuEnd = process.TotalProcessorTime;
        allocatedEnd = GC.GetTotalAllocatedBytes(false);
        gcEnd = Enumerable.Range(0, 3).Select(GC.CollectionCount).ToArray();
    }
    public void Export(Dictionary<string, object?> metrics, Dictionary<string, object?> runtime)
    {
        var seconds = (lastSample - started) / 1e9;
        metrics["process.cpuPercent"] = seconds > 0 ? (cpuEnd - cpuStart).TotalSeconds / seconds * 100 : 0;
        metrics["process.rssMb"] = rssMax / 1048576.0;
        metrics["process.allocatedMb"] = Math.Max(0, allocatedEnd - allocatedStart) / 1048576.0;
        for (var i = 0; i < 3; i++) metrics["gc.gen" + i] = DecimalText.Of((ulong)Math.Max(0, gcEnd[i] - gcStart[i]));
        runtime["rssSampling"] = new { name = "Process.WorkingSet64", unit = "ns", type = "sampling",
            value = new { requestedIntervalNs = "100000000", actualIntervalsNs = sampleIntervals.Select(DecimalText.Of).ToArray(),
                startedTicks = DecimalText.Of(started), endedTicks = DecimalText.Of(lastSample) } };
        runtime["cpuObservationSeconds"] = new { name = "Process.TotalProcessorTime observation span", unit = "s", type = "number", value = seconds };
    }
    public void Dispose() => process.Dispose();
}
