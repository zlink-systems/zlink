using Xunit;

namespace ZLink.Framework.Perf.Tests;

public sealed class FanoutReceiptTests
{
    [Fact]
    public async Task SubscriberUsesHandlerEntryTickWhenFinalOriginalIsCollectedAfterTheWindow()
    {
        var workload = TestWorkload.Create(.03, .03);
        var config = new RoleConfig("test", "pubsub-fanout-echo/1024/test", new string('f', 64), "subscriber", 0,
            "pubsub-fanout-echo", "clientserver", "fanout", null, [], null, "", "", false, "None", null, [], [],
            "Framework default", workload, []);
        using var measurement = new Measurement(config, false);
        var directory = Path.Combine(Path.GetTempPath(), "fanout-entry-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var receipts = new FanoutReceipts(null!, measurement, new ObjectsReadiness(true, ""), directory);
            Assert.True(measurement.Start(new PerfTriggerRequest { runId = "test", cellId = config.cellId,
                phase = "warmup", resetSeq = "0" }, null).accepted);
            await measurement.PhaseTask;
            Assert.True(measurement.Reset(new ResetRequest { runId = "test", cellId = config.cellId, resetSeq = "1" }, null).ok);
            Assert.True(measurement.Start(new PerfTriggerRequest { runId = "test", cellId = config.cellId,
                phase = "measured", resetSeq = "1" }, null).accepted);
            var message = new PerfPublishEvent
            {
                runId = config.runId, cellId = config.cellId, resetSeq = "1", phase = "measured",
                sequence = "42", topic = FanoutMetrics.Topic, sentTicks = "0",
                clockDomainId = PerfClock.Domain, payload = measurement.Pattern.Base64
            };
            var startTicks = measurement.StartTicks;
            var endTicks = measurement.EndTicks;
            receipts.Record(message with { sequence = "41" }, PerfClock.Now);
            await measurement.PhaseTask;

            receipts.Record(message with { sequence = "42" }, startTicks);
            receipts.Record(message with { sequence = "43" }, endTicks - 1);
            receipts.Record(message with { sequence = "44" }, endTicks);
            measurement.FinalSnapshot = true;
            var snapshot = measurement.Snapshot(null);
            var original = PerfJson.Read<SubscriberSequences>(File.ReadAllText(Path.Combine(directory, "subscriber-0-sequences.json")));
            Assert.Equal(new SequenceRange(41, 43), Assert.Single(original.windowRanges));
            using var receiptMetrics = System.Text.Json.JsonDocument.Parse(PerfJson.Write(snapshot.runtimeMetrics["fanoutReceipts"]));
            Assert.Equal("1", receiptMetrics.RootElement.GetProperty("value").GetProperty("measuredOutsideWindow").GetString());
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    [Fact]
    public async Task FinalSnapshotDoesNotAddASecondReceiptClassificationRule()
    {
        var workload = TestWorkload.Create(.03, .03);
        var config = new RoleConfig("test", "pubsub-fanout-echo/1024/seal", new string('e', 64), "subscriber", 0,
            "pubsub-fanout-echo", "clientserver", "fanout", null, [], null, "", "", false, "None", null, [], [],
            "Framework default", workload, []);
        using var measurement = new Measurement(config, false);
        var directory = Path.Combine(Path.GetTempPath(), "fanout-seal-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var receipts = new FanoutReceipts(null!, measurement, new ObjectsReadiness(true, ""), directory);
            Assert.True(measurement.Start(new PerfTriggerRequest { runId = "test", cellId = config.cellId,
                phase = "warmup", resetSeq = "0" }, null).accepted);
            await measurement.PhaseTask;
            Assert.True(measurement.Reset(new ResetRequest { runId = "test", cellId = config.cellId, resetSeq = "1" }, null).ok);
            Assert.True(measurement.Start(new PerfTriggerRequest { runId = "test", cellId = config.cellId,
                phase = "measured", resetSeq = "1" }, null).accepted);
            var receivedTicks = measurement.StartTicks;
            await measurement.PhaseTask;
            measurement.FinalSnapshot = true;
            _ = measurement.Snapshot(null);
            measurement.FinalSnapshot = false;
            receipts.Record(new PerfPublishEvent
            {
                runId = config.runId, cellId = config.cellId, resetSeq = "1", phase = "measured",
                sequence = "7", topic = FanoutMetrics.Topic, sentTicks = "0",
                clockDomainId = PerfClock.Domain, payload = measurement.Pattern.Base64
            }, receivedTicks);
            using var receiptMetrics = System.Text.Json.JsonDocument.Parse(
                PerfJson.Write(measurement.Snapshot(null).runtimeMetrics["fanoutReceipts"]));
            Assert.Equal("1", receiptMetrics.RootElement.GetProperty("value").GetProperty("uniqueInWindow").GetString());
        }
        finally { Directory.Delete(directory, recursive: true); }
    }
}
