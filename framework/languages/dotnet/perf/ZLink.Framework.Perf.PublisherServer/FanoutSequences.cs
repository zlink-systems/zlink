using System.Collections.Concurrent;
using System.Numerics;

namespace ZLink.Framework.Perf;

// Perf spec §15.4: sequence originals of the PS cells. The Publisher and Subscriber role projects compile this file
// (the Subscriber links it), so the original format has one definition.
public sealed record SequenceRange(ulong first, ulong last);

public sealed record PublisherSequences : Identity
{
    public required SequenceRange[] attemptedRanges { get; init; }
    public required SequenceRange[] windowSuccessRanges { get; init; }
}

public sealed record SubscriberSequences : Identity
{
    public required int subscriberId { get; init; }
    public required SequenceRange[] windowRanges { get; init; }
    public required ulong duplicateEvents { get; init; }
    public required Dictionary<string, NullReason> nullReasons { get; init; }
    public required object[]? timingEvidence { get; init; }
}

// A set of U64 sequences kept as a chunked bit set: one bit per sequence, safe for concurrent writers.
public sealed class SequenceBitSet
{
    private const int ChunkBits = 1 << 18;
    private const int ChunkWords = ChunkBits / 64;
    private readonly ConcurrentDictionary<ulong, ulong[]> chunks = new();
    private long count;
    public ulong Count => (ulong)Interlocked.Read(ref count);
    public ulong RetainedBytes => (ulong)chunks.Count * ChunkWords * sizeof(ulong);

    // False when the sequence was already in the set.
    public bool TrySet(ulong sequence)
    {
        var chunk = chunks.GetOrAdd(sequence / ChunkBits, static _ => new ulong[ChunkWords]);
        var bit = (int)(sequence % ChunkBits);
        var mask = 1UL << (bit & 63);
        if ((Interlocked.Or(ref chunk[bit >> 6], mask) & mask) != 0) return false;
        Interlocked.Increment(ref count);
        return true;
    }
    public bool Contains(ulong sequence)
    {
        if (!chunks.TryGetValue(sequence / ChunkBits, out var chunk)) return false;
        var bit = (int)(sequence % ChunkBits);
        return (Volatile.Read(ref chunk[bit >> 6]) & (1UL << (bit & 63))) != 0;
    }
    // Maximal contiguous intervals, ascending, both ends inclusive (§15.4).
    public SequenceRange[] Ranges()
    {
        List<SequenceRange> ranges = [];
        ulong? open = null;
        ulong next = 0; // the sequence expected next while an interval is open
        foreach (var index in chunks.Keys.Order())
        {
            var chunk = chunks[index];
            var origin = index * ChunkBits;
            for (var word = 0; word < ChunkWords; word++)
            {
                var bits = Volatile.Read(ref chunk[word]);
                var position = origin + (ulong)word * 64;
                if (bits == 0) { Close(); continue; }
                while (bits != 0)
                {
                    var low = BitOperations.TrailingZeroCount(bits);
                    var run = BitOperations.TrailingZeroCount(~(bits >> low));
                    var first = position + (ulong)low;
                    if (open is null) open = first;
                    else if (first != next) { Close(); open = first; }
                    next = first + (ulong)run;
                    bits = run + low >= 64 ? 0 : bits & ~(((1UL << run) - 1) << low);
                }
                // An interval that stops before the end of this word is closed by the next gap or the end of input.
                if ((Volatile.Read(ref chunk[word]) >> 63) == 0) Close();
            }
        }
        Close();
        return ranges.ToArray();

        void Close()
        {
            if (open is not null) ranges.Add(new(open.Value, next - 1));
            open = null;
        }
    }
}

// Metric keys a PS role owns or hands to the runner's sequence intersection (§14, §15.4).
public static class FanoutMetrics
{
    public const string Topic = "perf.echo";
    private static readonly string[] IntersectionKeys = ["fanout.subscriberCount",
        "fanout.deliveredInWindow", "fanout.outOfCohortEvents", "fanout.deliveryRatio",
        "fanout.deliveryOpsPerSec"];
    private static readonly string[] EchoKeys = ["messages.completed", "throughput.kops"];

    public static void Value(PerfMetricsSnapshot snapshot, string key, object? value)
    {
        snapshot.metrics[key] = value;
        snapshot.nullReasons.Remove("/metrics/" + key);
    }
    public static void Null(PerfMetricsSnapshot snapshot, string key, string code, string reason) =>
        MetricCatalog.Null(snapshot.metrics, snapshot.nullReasons, "metrics", key, code, reason);

    // Every PS role: the echo outcomes and echo latency do not apply (§10.11); delivery is intersected by the runner.
    public static void ApplyCommon(PerfMetricsSnapshot snapshot, bool hasDeliveryOwner)
    {
        foreach (var key in IntersectionKeys)
            Null(snapshot, key, "NOT_APPLICABLE", "Delivery counts come from the runner's intersection of the publisher and subscriber sequence originals (§15.4).");
        foreach (var prefix in new[] { "latency" })
            foreach (var suffix in MetricCatalog.LatencySuffixes)
                Null(snapshot, prefix + "." + suffix, "NOT_APPLICABLE", "A fanout cell has no echo round trip (§10.11).");
        foreach (var key in new[] { "latencyMs" })
        {
            MetricCatalog.Null(snapshot.histograms, snapshot.nullReasons, "histograms", key, "NOT_APPLICABLE", "A fanout cell has no echo round trip (§10.11).");
            snapshot.nullReasons.Remove("/histograms/" + key + "/maxNs");
        }
        foreach (var key in EchoKeys)
            Null(snapshot, key, "NOT_APPLICABLE", "A fanout cell records publish admission, not echo completion (§10.11).");
        foreach (var prefix in new[] { "fanout.deliveryLatency" })
            foreach (var suffix in MetricCatalog.LatencySuffixes)
                Null(snapshot, prefix + "." + suffix, hasDeliveryOwner ? "CLOCK_DOMAIN_UNVERIFIED" : "NOT_APPLICABLE",
                    hasDeliveryOwner ? "Publisher and Subscriber use process-local monotonic clocks; no shared clock domain is verified (§15.2)."
                        : "Delivery latency is observed by Subscriber processes.");
        foreach (var key in new[] { "fanoutDeliveryLatencyMs" })
            MetricCatalog.Null(snapshot.histograms, snapshot.nullReasons, "histograms", key,
                hasDeliveryOwner ? "CLOCK_DOMAIN_UNVERIFIED" : "NOT_APPLICABLE",
                hasDeliveryOwner ? "No verified shared clock domain between Publisher and Subscriber processes (§15.2)."
                    : "Delivery latency is observed by Subscriber processes.");
    }

    public static void WriteOnce(string path, object original)
    {
        using var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read);
        using var writer = new StreamWriter(stream);
        writer.Write(PerfJson.Write(original));
        writer.Write('\n');
    }
}
