using Xunit;

namespace ZLink.Framework.Perf.Tests;

public sealed class FanoutSequencesTests
{
    private static SequenceBitSet Of(params ulong[] sequences)
    {
        var set = new SequenceBitSet();
        foreach (var sequence in sequences) set.TrySet(sequence);
        return set;
    }
    private static string Show(SequenceRange[] ranges) => string.Join(",", ranges.Select(r => $"{r.first}-{r.last}"));

    [Fact]
    public void RangesAreMaximalAcrossWordAndChunkBoundaries()
    {
        Assert.Equal("5-7,9-9,63-65,262143-262144,1000000-1000000",
            Show(Of(5, 6, 7, 9, 63, 64, 65, 262143, 262144, 1_000_000).Ranges()));
        var full = new SequenceBitSet();
        for (ulong i = 0; i < 200; i++) full.TrySet(i);
        Assert.Equal("0-199", Show(full.Ranges()));
        Assert.Equal("", Show(new SequenceBitSet().Ranges()));
        // A gap of one sequence must split the interval; a wrong boundary test would merge or shift it.
        Assert.Equal("62-62,64-64", Show(Of(62, 64).Ranges()));
        Assert.NotEqual("62-64", Show(Of(62, 64).Ranges()));
        Assert.Equal("0-0,18446744073709551615-18446744073709551615", Show(Of(0, ulong.MaxValue).Ranges()));
    }
    [Fact]
    public void TrySetReportsDuplicatesAndCountsUniqueSequences()
    {
        var set = new SequenceBitSet();
        Assert.True(set.TrySet(10));
        Assert.False(set.TrySet(10));
        Assert.True(set.Contains(10));
        Assert.False(set.Contains(11));
        Assert.Equal(1UL, set.Count);
    }
    [Fact]
    public void ConcurrentWritersLoseNoSequenceAndCountEachDuplicateOnce()
    {
        var set = new SequenceBitSet();
        var fresh = 0L;
        Parallel.For(0, 8, _ =>
        {
            for (ulong i = 0; i < 300_000; i++)
                if (set.TrySet(i)) Interlocked.Increment(ref fresh);
        });
        Assert.Equal(300_000L, fresh);
        Assert.Equal(300_000UL, set.Count);
        Assert.Equal("0-299999", Show(set.Ranges()));
    }
    [Fact]
    public void AnOriginalIsWrittenOnceAndNeverReplaced()
    {
        var path = Path.Combine(Path.GetTempPath(), "fanout-" + Guid.NewGuid().ToString("N") + ".json");
        try
        {
            FanoutMetrics.WriteOnce(path, new { value = 1 });
            FanoutMetrics.WriteOnce(path, new { value = 2 });
            Assert.Contains("1", File.ReadAllText(path));
            Assert.DoesNotContain("2", File.ReadAllText(path));
        }
        finally { File.Delete(path); }
    }
    [Fact]
    public void FanoutRolesReplaceEchoMetricsWithReasonedNullsAndTheEventRow()
    {
        var config = new RoleConfig("test", "pubsub-fanout-echo/1024/x", new string((char)97, 64), "publisher", 0, "pubsub-fanout-echo",
            null, null, null, [], null, "", "", true, "None", null, [], [], "Framework default",
            new(1024, .05, .05, 1, null, 1, 1, null, 1000, 1000, 5000, 30000, 5000, 1000), []);
        using var measurement = new Measurement(config, true);
        measurement.MessageTypes = [("event", nameof(PerfPublishEvent))];
        var snapshot = measurement.Snapshot(null);
        foreach (var owner in new[] { true, false })
        {
            FanoutMetrics.ApplyCommon(snapshot, hasDeliveryOwner: owner);
            foreach (var key in new[] { "messages.completed", "throughput.kops", "latency.p99Ms", "fanout.deliveryRatio", "fanout.deliveryLatency.p50Ms" })
            {
                Assert.Null(snapshot.metrics[key]);
                Assert.False(string.IsNullOrEmpty(snapshot.nullReasons["/metrics/" + key].reason));
            }
            Assert.Equal(owner ? "CLOCK_DOMAIN_UNVERIFIED" : "NOT_APPLICABLE", snapshot.nullReasons["/metrics/fanout.deliveryLatency.p50Ms"].code);
            Assert.All(snapshot.serializedMessageBytes, row => Assert.Equal(("event", nameof(PerfPublishEvent)), (row.direction, row.packetName)));
            Assert.DoesNotContain("/histograms/latencyMs/maxNs", snapshot.nullReasons.Keys);
        }
        FanoutMetrics.Value(snapshot, "messages.published", "7");
        Assert.Equal("7", snapshot.metrics["messages.published"]);
        Assert.DoesNotContain("/metrics/messages.published", snapshot.nullReasons.Keys);
    }
}
