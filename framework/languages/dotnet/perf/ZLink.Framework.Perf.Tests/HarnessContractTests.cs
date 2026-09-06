using System.Text.Json;
using Xunit;

namespace ZLink.Framework.Perf.Tests;

public sealed class HarnessContractTests
{
    private static RoleConfig Config(double seconds = .05) => new("test", "session-echo-only/1024/test", new string('a', 64),
        "client", 0, "session-echo-only", null, null, null, null, null, "", "", false, "None", null, [], [],
        "Immediate", new(1024, seconds, seconds, 1, 1, null, 1, 1, 1000, 1000, 5000, 30000, 5000, 1000), []);
    private static PerfTriggerRequest Trigger(Measurement measurement, string phase, string seq) => new()
    { runId = measurement.Config.runId, cellId = measurement.Config.cellId, phase = phase, resetSeq = seq };
    private static ResetRequest Reset(Measurement measurement, string seq = "1") => new()
    { runId = measurement.Config.runId, cellId = measurement.Config.cellId, resetSeq = seq };

    [Fact]
    public void PayloadValidatesEveryByteAndCanonicalPaddedBase64()
    {
        foreach (var size in new[] { 1024, 4096 })
        {
            var pattern = new PayloadPattern(size);
            pattern.Validate(pattern.Base64);
            var bytes = Convert.FromBase64String(pattern.Base64);
            Assert.Equal(size, bytes.Length);
            Assert.Equal(29, bytes[0]);
            bytes[size - 1] ^= 1;
            Assert.Throws<PerfValidationException>(() => pattern.Validate(Convert.ToBase64String(bytes)));
            Assert.Throws<PerfValidationException>(() => pattern.Validate(pattern.Base64 + "\n"));
        }
    }
    [Fact]
    public void HistogramKeepsExactSumOverflowAndInclusiveBounds()
    {
        var histogram = new Histogram();
        histogram.Record(100_000);
        histogram.Record(100_001);
        histogram.Record(1_024_000_001);
        Dictionary<string, object?> metrics = [], histograms = [];
        Dictionary<string, NullReason> reasons = [];
        histogram.Export("latencyMs", "latency", metrics, histograms, reasons);
        var snapshot = histogram.Snapshot();
        Assert.Equal("1", snapshot.counts[0]);
        Assert.Equal("1", snapshot.counts[1]);
        Assert.Equal("1", snapshot.overflow);
        Assert.Equal("3", snapshot.count);
        Assert.Equal("1024200002", snapshot.sumNs);
        Assert.Equal(.25, metrics["latency.p50Ms"]);
        Assert.Null(metrics["latency.p95Ms"]);
        Assert.Equal("HISTOGRAM_OVERFLOW", reasons["/metrics/latency.p95Ms"].code);
        Assert.Equal(1024, reasons["/metrics/latency.p95Ms"].lowerBoundMs);
        Assert.Equal(1024.000001, metrics["latency.maxMs"]);
    }
    [Fact]
    public void EmptyHistogramHasReasonsForEveryLatencyAndMax()
    {
        Dictionary<string, object?> metrics = [], histograms = [];
        Dictionary<string, NullReason> reasons = [];
        new Histogram().Export("latencyMs", "latency", metrics, histograms, reasons);
        Assert.All(metrics.Values, Assert.Null);
        Assert.Equal(6, reasons.Count);
        Assert.All(reasons.Values, value => Assert.Equal("NO_SAMPLES", value.code));
    }
    [Theory]
    [InlineData("01")]
    [InlineData("+1")]
    [InlineData("-0")]
    [InlineData("18446744073709551616")]
    public void DecimalU64RejectsNoncanonicalOrOutOfRange(string value) => Assert.Throws<JsonException>(() => DecimalText.U64(value));
    [Fact]
    public void JsonPreservesAll64BitsAndRejectsNumberTokens()
    {
        var json = PerfJson.Write(new { unsigned = ulong.MaxValue, signed = long.MinValue });
        Assert.Contains("\"18446744073709551615\"", json);
        Assert.Contains("\"-9223372036854775808\"", json);
        Assert.Equal(ulong.MaxValue, PerfJson.Read<ulong>("\"18446744073709551615\""));
        Assert.Throws<JsonException>(() => PerfJson.Read<ulong>("18446744073709551615"));
        Assert.Throws<JsonException>(() => PerfJson.Read<ResetRequest>("{\"runId\":\"a\",\"cellId\":\"b\"}"));
        Assert.Throws<JsonException>(() => PerfJson.Read<ResetRequest>("{\"runId\":\"a\",\"cellId\":\"b\",\"resetSeq\":1}"));
    }
    [Fact]
    public async Task ResetRejectsAnOutstandingWarmupAndMeasuredCannotStartBeforeReset()
    {
        using var measurement = new Measurement(Config(), true);
        var outstanding = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(measurement.Start(Trigger(measurement, "warmup", "0"), async () =>
        {
            Assert.True(measurement.BeginOperation(out var started));
            await outstanding.Task;
            measurement.CompleteOperation(started);
        }).accepted);
        await Task.Delay(80); // Test controls a pending operation beyond the warmup window.
        Assert.False(measurement.Reset(Reset(measurement), null).ok);
        Assert.False(measurement.Start(Trigger(measurement, "measured", "1"), null).accepted);
        outstanding.SetResult();
        await measurement.PhaseTask;
        var ack = measurement.Reset(Reset(measurement), null);
        Assert.True(ack.ok);
        Assert.Same(ack, measurement.Reset(Reset(measurement), null));
        Assert.False(measurement.Reset(Reset(measurement, "0"), null).ok);
        Assert.True(measurement.Start(Trigger(measurement, "measured", "1"), null).accepted);
        Assert.False(measurement.Reset(Reset(measurement), null).ok);
        await measurement.PhaseTask;
        Assert.Same(ack, measurement.Reset(Reset(measurement), null));
    }
    [Fact]
    public async Task PublicStatusSamplingDoesNotHoldTheApplicationCounterLock()
    {
        using var measurement = new Measurement(Config(.12), true);
        measurement.SamplePublicState = () =>
        {
            var read = Task.Run(() => measurement.Phase);
            Assert.True(read.Wait(TimeSpan.FromSeconds(1)), "Public status observation held the application counter lock.");
            return new { phase = read.Result };
        };
        Assert.True(measurement.Start(Trigger(measurement, "warmup", "0"), null).accepted);
        await measurement.PhaseTask;
    }
    [Fact]
    public async Task SettleSuccessIsExcludedFromWindowRateAndDuplicateStartRunsOnce()
    {
        using var measurement = new Measurement(Config(.08), true);
        measurement.Start(Trigger(measurement, "warmup", "0"), null);
        await measurement.PhaseTask;
        Assert.True(measurement.Reset(Reset(measurement), null).ok);
        var pending = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var calls = 0;
        Func<Task> workload = async () =>
        {
            Interlocked.Increment(ref calls);
            Assert.True(measurement.BeginOperation(out var started));
            entered.SetResult();
            await pending.Task;
            measurement.CompleteOperation(started);
        };
        var trigger = Trigger(measurement, "measured", "1");
        Assert.True(measurement.Start(trigger, workload).accepted);
        Assert.Equal("alreadyStarted", measurement.Start(trigger, workload).state);
        await entered.Task;
        await Task.Delay(100);
        Assert.False(measurement.BeginOperation(out _));
        pending.SetResult();
        await measurement.PhaseTask;
        var snapshot = measurement.Snapshot(null);
        Assert.Equal(1, calls);
        Assert.Equal("1", snapshot.metrics["messages.sent"]);
        Assert.Equal("0", snapshot.metrics["messages.completed"]);
        Assert.Equal("1", snapshot.metrics["messages.settleCompleted"]);
        Assert.Equal("0", snapshot.metrics["messages.unresolved"]);
        Assert.Equal(0.0, snapshot.metrics["throughput.kops"]);
        Assert.Equal("0", Assert.IsType<HistogramSnapshot>(snapshot.histograms["latencyMs"]).count);
        Assert.Equal("1", Assert.IsType<HistogramSnapshot>(snapshot.histograms["settleLatencyMs"]).count);
    }
}
