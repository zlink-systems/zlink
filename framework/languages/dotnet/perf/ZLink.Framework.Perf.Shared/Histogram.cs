using System.Globalization;
using System.Numerics;

namespace ZLink.Framework.Perf;

public sealed record HistogramSnapshot(string unit, string ticksUnit, double[] bounds, string[] counts,
    string overflow, string count, string sumNs, string? maxNs, string percentileMethod);

public sealed class Histogram
{
    public static readonly double[] Bounds = PerfJson.Read<double[]>(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "histogram-bounds.json")));
    private static readonly long[] BoundsNs = Bounds.Select(value => checked((long)(value * 1_000_000))).ToArray();
    private readonly ulong[] counts = new ulong[Bounds.Length];
    private ulong count, overflow, max;
    private BigInteger sum;
    public void Record(long elapsedNs)
    {
        if (elapsedNs < 0) throw new ArgumentOutOfRangeException(nameof(elapsedNs));
        checked
        {
            count++;
            var bucket = 0;
            while (bucket < BoundsNs.Length && elapsedNs > BoundsNs[bucket]) bucket++;
            if (bucket == BoundsNs.Length) overflow++; else counts[bucket]++;
        }
        sum += elapsedNs;
        max = Math.Max(max, (ulong)elapsedNs);
    }
    public HistogramSnapshot Snapshot() => new("ms", "ns", Bounds.ToArray(), counts.Select(DecimalText.Of).ToArray(),
        DecimalText.Of(overflow), DecimalText.Of(count), sum.ToString(CultureInfo.InvariantCulture),
        count == 0 ? null : DecimalText.Of(max), "nearest-rank-bucket-upper-bound");
    public void Export(string histogramKey, string metricPrefix, Dictionary<string, object?> metrics,
        Dictionary<string, object?> histograms, Dictionary<string, NullReason> reasons)
    {
        histograms[histogramKey] = Snapshot();
        foreach (var name in new[] { "meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs" })
        {
            var key = metricPrefix + "." + name;
            double? value = count == 0 ? null : name switch
            {
                "meanMs" => (double)sum / count / 1_000_000,
                "maxMs" => max / 1_000_000.0,
                _ => Percentile(int.Parse(name.AsSpan(1, 2), CultureInfo.InvariantCulture))
            };
            metrics[key] = value;
            if (value is null) reasons["/metrics/" + key] = count == 0
                ? new("NO_SAMPLES", "No successful samples in this cohort and window.")
                : new("HISTOGRAM_OVERFLOW", "Nearest rank lies above the final bucket.", lowerBoundMs: 1024);
        }
        if (count == 0) reasons["/histograms/" + histogramKey + "/maxNs"] =
            new("NO_SAMPLES", "No successful samples in this cohort and window.");
    }
    private double? Percentile(int percentile)
    {
        var rank = ((BigInteger)percentile * count + 99) / 100;
        BigInteger cumulative = 0;
        for (var i = 0; i < counts.Length; i++)
        {
            cumulative += counts[i];
            if (cumulative >= rank) return Bounds[i];
        }
        return null;
    }
}
