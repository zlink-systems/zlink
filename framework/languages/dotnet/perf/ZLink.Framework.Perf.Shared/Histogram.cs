using System.Globalization;
using System.Numerics;

namespace ZLink.Framework.Perf;

public sealed record HistogramSnapshot(string unit, string ticksUnit, string bucketSpec, string[] boundsNs,
    string[] counts, string overflow, string count, string sumNs, string? maxNs, string percentileMethod);

public sealed class Histogram
{
    public const string BucketSpec = "ns-1us-1pct-60s-v1";
    public static readonly long[] BoundsNs = GenerateBounds();
    private readonly ulong[] counts = new ulong[BoundsNs.Length];
    private ulong count, overflow, max;
    private BigInteger sum;
    private static long[] GenerateBounds()
    {
        List<long> bounds = [1000];
        while (bounds[^1] < 60_000_000_000) bounds.Add(Math.Min(60_000_000_000, checked((bounds[^1] * 101 + 99) / 100)));
        return bounds.ToArray();
    }
    public static void ValidateFixture()
    {
        using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "histogram-bounds-ns.json")));
        var root = doc.RootElement;
        var values = root.ValueKind == System.Text.Json.JsonValueKind.Array ? root : root.GetProperty("boundsNs");
        var fixture = values.EnumerateArray().Select(v => v.ValueKind == System.Text.Json.JsonValueKind.String ?
            DecimalText.I64(v.GetString()!) : v.GetInt64()).ToArray();
        if (!fixture.SequenceEqual(BoundsNs)) throw new PerfValidationException("SchemaMismatch", "Common histogram fixture differs.");
    }
    public void Record(long elapsedNs)
    {
        if (elapsedNs < 0) throw new ArgumentOutOfRangeException(nameof(elapsedNs));
        checked
        {
            count++;
            var bucket = Array.BinarySearch(BoundsNs, elapsedNs);
            if (bucket < 0) bucket = ~bucket;
            if (bucket == BoundsNs.Length) overflow++; else counts[bucket]++;
        }
        sum += elapsedNs;
        max = Math.Max(max, (ulong)elapsedNs);
    }
    public HistogramSnapshot Snapshot() => new("ms", "ns", BucketSpec, BoundsNs.Select(DecimalText.Of).ToArray(),
        counts.Select(DecimalText.Of).ToArray(), DecimalText.Of(overflow), DecimalText.Of(count),
        sum.ToString(CultureInfo.InvariantCulture), count == 0 ? null : DecimalText.Of(max), "nearest-rank-bucket-upper-bound");
    public void Export(string histogramKey, string metricPrefix, Dictionary<string, object?> metrics,
        Dictionary<string, object?> histograms, Dictionary<string, NullReason> reasons)
    {
        histograms[histogramKey] = Snapshot();
        reasons.Remove("/histograms/" + histogramKey);
        foreach (var name in MetricCatalog.LatencySuffixes)
        {
            var key = metricPrefix + "." + name;
            var insufficient = name == "p999Ms" && count < 100_000;
            double? value = count == 0 || insufficient ? null : name switch
            {
                "meanMs" => (double)sum / count / 1_000_000,
                "maxMs" => max / 1_000_000.0,
                "p999Ms" => Percentile(999), "p99Ms" => Percentile(990),
                "p95Ms" => Percentile(950), _ => Percentile(500)
            };
            metrics[key] = value;
            reasons.Remove("/metrics/" + key);
            if (value is null) reasons["/metrics/" + key] = insufficient
                ? new("INSUFFICIENT_SAMPLES", "p99.9 requires at least 100000 samples.")
                : count == 0 ? new("NO_SAMPLES", "No successful samples in this cohort and window.")
                : new("HISTOGRAM_OVERFLOW", "Nearest rank lies above the final bucket.", lowerBoundMs: 60000);
        }
        if (count == 0) reasons["/histograms/" + histogramKey + "/maxNs"] = new("NO_SAMPLES", "No successful samples.");
    }
    private double? Percentile(int perThousand)
    {
        var rank = ((BigInteger)perThousand * count + 999) / 1000;
        BigInteger cumulative = 0;
        for (var i = 0; i < counts.Length; i++)
        {
            cumulative += counts[i];
            if (cumulative >= rank) return BoundsNs[i] / 1_000_000.0;
        }
        return null;
    }
}
