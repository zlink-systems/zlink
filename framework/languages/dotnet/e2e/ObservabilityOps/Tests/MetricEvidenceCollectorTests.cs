using System.Diagnostics;
using System.Diagnostics.Metrics;
using ObservabilityOps.Server.Support;
using ObservabilityOps.Shared;
using Xunit;
using Zlink.Framework.E2E.Diagnostics;

public sealed class MetricEvidenceCollectorTests
{
    [Fact]
    public void MessageFlowListener_Captures_Standard_Activity_Tags_And_File_Format()
    {
        var path = Path.Combine(
            Path.GetTempPath(),
            $"zlink-message-flow-{Guid.NewGuid():N}.log");
        E2eMessageFlow? captured = null;
        try
        {
            using var listener = new E2eMessageFlowListener(
                path,
                "test-node",
                flow => captured = flow);
            using var source = new ActivitySource(
                E2eMessageFlowListener.ActivitySourceName);
            using (var activity = source.StartActivity("zlink.message_flow"))
            {
                Assert.NotNull(activity);
                activity.SetTag("phase", "error");
                activity.SetTag("surface", "Channel");
                activity.SetTag("message_kind", "Request");
                activity.SetTag("packet_name", "MissingReq");
                activity.SetTag("flow_id", "flow-1");
                activity.SetTag("reason", "HandlerMissing");
                activity.SetTag("action", "ReplyError");
            }

            Assert.NotNull(captured);
            Assert.Equal("error", captured.Phase);
            Assert.Equal("Channel", captured.Surface);
            Assert.Equal("Request", captured.MessageKind);
            Assert.Equal("MissingReq", captured.PacketName);
            Assert.Equal("flow-1", captured.FlowId);
            var line = Assert.Single(File.ReadAllLines(path));
            Assert.Contains("label=test-node", line, StringComparison.Ordinal);
            Assert.Contains("packet=MissingReq", line, StringComparison.Ordinal);
            Assert.Contains("flow=flow-1", line, StringComparison.Ordinal);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void MessageFlowListener_Isolates_Evidence_Sink_Failures()
    {
        using var listener = new E2eMessageFlowListener(
            filePath: null,
            label: null,
            onFlow: _ => throw new InvalidOperationException("sink failure"));
        using var source = new ActivitySource(
            E2eMessageFlowListener.ActivitySourceName);

        var error = Record.Exception(() =>
        {
            using var activity = source.StartActivity("zlink.message_flow");
            Assert.NotNull(activity);
            activity.SetTag("phase", "error");
        });

        Assert.Null(error);
    }

    [Fact]
    public void Snapshot_Preserves_Instrument_Semantics_And_Replaces_Observable_Series()
    {
        using var collector = new MetricEvidenceCollector();
        using var meter = new Meter("zlink.framework");
        var counter = meter.CreateCounter<long>("test.counter");
        var active = meter.CreateUpDownCounter<long>("test.active");
        var duration = meter.CreateHistogram<double>("test.duration");
        var observableState = "serving";
        _ = meter.CreateObservableGauge(
            "test.state",
            () => new Measurement<long>(
                1,
                new KeyValuePair<string, object?>("state", observableState)));

        counter.Add(2);
        counter.Add(3);
        active.Add(2);
        active.Add(-1);
        duration.Record(2);
        duration.Record(4);
        var first = collector.Snapshot();
        Assert.Equal(5m, Single(first, "test.counter").Value);
        Assert.Equal(1m, Single(first, "test.active").Value);
        var histogram = Single(first, "test.duration");
        Assert.Equal(2, histogram.Count);
        Assert.Equal(6m, histogram.Sum);
        Assert.Equal(2m, histogram.Min);
        Assert.Equal(4m, histogram.Max);
        Assert.Equal("serving", Single(first, "test.state").Tags["state"]);

        observableState = "draining";
        var second = collector.Snapshot();
        var stateSeries = second.Where(sample => sample.Name == "test.state").ToArray();
        Assert.Single(stateSeries);
        Assert.Equal("draining", stateSeries[0].Tags["state"]);
    }

    [Fact]
    public void Snapshot_Aggregates_Providers_And_Preserves_Long_Precision()
    {
        using var collector = new MetricEvidenceCollector();
        using var firstMeter = new Meter("zlink.framework");
        using var secondMeter = new Meter("zlink.framework");
        _ = firstMeter.CreateObservableGauge("test.aggregate", () => 2L);
        _ = secondMeter.CreateObservableGauge("test.aggregate", () => 3L);
        var exact = firstMeter.CreateCounter<long>("test.exact-long");
        exact.Add(9_007_199_254_740_993L);

        var snapshot = collector.Snapshot();

        Assert.Equal(5m, Single(snapshot, "test.aggregate").Value);
        Assert.Equal(9_007_199_254_740_993m, Single(snapshot, "test.exact-long").Value);
    }

    private static MetricSample Single(IEnumerable<MetricSample> samples, string name) =>
        Assert.Single(samples.Where(sample => sample.Name == name));
}
