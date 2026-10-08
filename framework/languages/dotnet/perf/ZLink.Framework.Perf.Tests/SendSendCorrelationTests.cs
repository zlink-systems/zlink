using Xunit;

namespace ZLink.Framework.Perf.Tests;

// §13: the harness correlation of a send/send operation and the family metrics that hang off it.
public sealed class SendSendCorrelationTests
{
    private const int TestSetupTimeoutMs = 60;

    private static RoleConfig Config(int testSetupTimeoutMs = TestSetupTimeoutMs) =>
        new(
            "test",
            "s2s-channel-to-spot-send-send-echo/4096/test",
            new string('b', 64),
            "channel",
            0,
            "s2s-channel-to-spot-send-send-echo",
            "routemesh",
            "ch",
            "mesh",
            [],
            null,
            "",
            "",
            true,
            "ObjectClient",
            null,
            [],
            [],
            "SpotWide",
            TestWorkload.Create(setupTimeoutMs: testSetupTimeoutMs),
            [],
            mode: "send-send"
        );

    private sealed class Fixture : IDisposable
    {
        public readonly Measurement Measurement;
        public readonly ScenarioMetrics Metrics;
        public readonly SendSendCorrelation Correlations;

        public Fixture(int testSetupTimeoutMs = TestSetupTimeoutMs)
        {
            Measurement = new Measurement(Config(testSetupTimeoutMs), true);
            Metrics = new ScenarioMetrics(Measurement);
            Correlations = new SendSendCorrelation(Measurement, Metrics);
        }

        public PerfEchoRequest Request(ulong sequence) =>
            Measurement.Request(0, sequence, probe: true) with
            {
                returnChannel = "ch",
            };

        public PerfEchoReply Reply(PerfEchoRequest request) =>
            PayloadPattern.Reply(request, PerfClock.Now);

        public string Count(string key) => (string)Measurement.Snapshot(null).metrics[key]!;

        public void Dispose() => Measurement.Dispose();
    }

    [Fact]
    public async Task FirstReplyIsTheResultAndLaterRepliesAreOnlyCounted()
    {
        using var f = new Fixture();
        var request = f.Request(1);
        var entry = f.Correlations.Register(request, PerfClock.Now);
        f.Correlations.FirstSendEnded(entry, null);
        f.Correlations.Reply(f.Reply(request));
        f.Correlations.Reply(f.Reply(request));
        var (error, completed) = await f.Correlations.CompleteAsync(entry);
        Assert.Null(error);
        Assert.True(completed >= entry.StartedTicks);
        Assert.Equal("1", f.Count("messages.duplicateReply"));
        Assert.Equal("0", f.Count("messages.lateReply"));
    }

    [Fact]
    public void ClosingAValidReplyReleasesTheRequestDto()
    {
        using var f = new Fixture();
        var request = f.Request(1);
        var entry = f.Correlations.Register(request, PerfClock.Now);

        f.Correlations.Reply(f.Reply(request));

        Assert.Null(entry.Request);
    }

    [Fact]
    public async Task EchoBeforeTheFirstSendTerminalKeepsItsOwnTime()
    {
        using var f = new Fixture();
        var request = f.Request(1);
        var entry = f.Correlations.Register(request, PerfClock.Now);
        f.Correlations.Reply(f.Reply(request));
        await Task.Delay(30);
        f.Correlations.FirstSendEnded(entry, null);
        var (error, completed) = await f.Correlations.CompleteAsync(entry);
        Assert.Null(error);
        Assert.True(
            PerfClock.Now - completed >= 25_000_000,
            "The echo time was replaced by the later send terminal."
        );
    }

    [Fact]
    public async Task ExpiryIsATimeoutAndALaterReplyIsLateNotASuccess()
    {
        using var f = new Fixture();
        var request = f.Request(1);
        var entry = f.Correlations.Register(request, PerfClock.Now);
        f.Correlations.FirstSendEnded(entry, null);
        var (error, _) = await f.Correlations.CompleteAsync(entry);
        var expired = Assert.IsType<PerfValidationException>(error);
        Assert.Equal("CorrelationExpired", expired.Kind);
        Assert.Null(entry.Request);
        f.Correlations.Reply(f.Reply(request));
        Assert.Equal("1", f.Count("messages.expired"));
        Assert.Equal("1", f.Count("messages.lateReply"));
        Assert.Equal("0", f.Count("messages.duplicateReply"));
        // The measurement classifies the expiry as a timeout, not a failure (expired is a subset of timeout).
        f.Measurement.Start(
            new PerfTriggerRequest
            {
                runId = "test",
                cellId = f.Measurement.Config.cellId,
                phase = "warmup",
                resetSeq = "0",
            },
            () =>
            {
                Assert.True(f.Measurement.BeginOperation(out var started, "send"));
                f.Measurement.CompleteOperation(started, error);
                return Task.CompletedTask;
            }
        );
        await f.Measurement.PhaseTask;
        var snapshot = f.Measurement.Snapshot(null);
        Assert.Equal("1", snapshot.metrics["messages.timeout"]);
        Assert.Equal("0", snapshot.metrics["messages.failed"]);
        Assert.Equal("1", snapshot.metrics["messages.sent"]);
    }

    [Fact]
    public async Task ReplyAfterDeadlineExpiresAtValidationCompletion()
    {
        const int TestReplyCorrelationExpiryMs = 30;
        using var f = new Fixture(TestReplyCorrelationExpiryMs);
        var request = f.Request(1);
        var entry = f.Correlations.Register(request, PerfClock.Now);
        while (PerfClock.Now < entry.ExpiresAtTicks)
            await Task.Delay(1);
        f.Correlations.Reply(f.Reply(request));
        var (error, completed) = await f.Correlations.CompleteAsync(entry);
        Assert.Equal("CorrelationExpired", Assert.IsType<PerfValidationException>(error).Kind);
        Assert.True(completed >= entry.ExpiresAtTicks);
        Assert.Equal("1", f.Count("messages.expired"));
        Assert.Equal("1", f.Count("messages.lateReply"));
    }

    [Fact]
    public async Task FirstSendTerminalAfterDeadlineUsesTheSameExpiryDecision()
    {
        using var f = new Fixture(20);
        var request = f.Request(1);
        var entry = f.Correlations.Register(request, PerfClock.Now);
        while (PerfClock.Now < entry.ExpiresAtTicks)
            await Task.Delay(1);
        f.Correlations.FirstSendEnded(entry, new InvalidOperationException("late send failure"));
        Assert.Equal(
            "CorrelationExpired",
            Assert
                .IsType<PerfValidationException>((await f.Correlations.CompleteAsync(entry)).Error)
                .Kind
        );
        Assert.Equal("1", f.Count("messages.expired"));
    }

    [Fact]
    public async Task SuccessfulFirstSendAfterDeadlineClosesExpiryBeforeCompletionWait()
    {
        using var f = new Fixture(20);
        var request = f.Request(1);
        var entry = f.Correlations.Register(request, PerfClock.Now);
        while (PerfClock.Now < entry.ExpiresAtTicks)
            await Task.Delay(1);
        f.Correlations.FirstSendEnded(entry, null);
        Assert.Equal("1", f.Count("messages.expired"));
        Assert.Equal(
            "CorrelationExpired",
            Assert
                .IsType<PerfValidationException>((await f.Correlations.CompleteAsync(entry)).Error)
                .Kind
        );
    }

    [Fact]
    public async Task OperationOwnerAccountsTheCorrelationResultOnce()
    {
        using var f = new Fixture();
        f.Metrics.Counters("driver.failed");
        Assert.True(
            f.Measurement.Start(
                new PerfTriggerRequest
                {
                    runId = "test",
                    cellId = f.Measurement.Config.cellId,
                    phase = "warmup",
                    resetSeq = "0",
                },
                async () =>
                {
                    var request = f.Measurement.Request(0, 1, probe: true);
                    Assert.True(f.Measurement.BeginOperation(out var started, "send"));
                    var entry = f.Correlations.Register(request, started);
                    f.Correlations.FirstSendEnded(entry, null);
                    f.Metrics.Count("driver.failed");
                    f.Correlations.Reply(f.Reply(request));
                    var (error, completed) = await f.Correlations.CompleteAsync(entry);
                    f.Measurement.CompleteOperation(started, error, completedTicks: completed);
                }
            ).accepted
        );
        await f.Measurement.PhaseTask;

        var snapshot = f.Measurement.Snapshot(null);
        Assert.Equal("1", snapshot.metrics["messages.sent"]);
        Assert.Equal("1", snapshot.metrics["messages.completed"]);
        Assert.Equal("0", snapshot.metrics["messages.failed"]);
        Assert.Equal("0", snapshot.metrics["messages.timeout"]);
        Assert.Equal("0", snapshot.metrics["messages.cancelled"]);
        Assert.Equal("0", snapshot.metrics["messages.inflightAtEnd"]);
        Assert.Equal("1", snapshot.metrics["driver.failed"]);
    }

    [Fact]
    public async Task AFailedFirstSendIsTheFinalResultUnlessTheEchoWasFirst()
    {
        using var f = new Fixture();
        var failedSend = f.Request(1);
        var entry = f.Correlations.Register(failedSend, PerfClock.Now);
        var boom = new InvalidOperationException("send failed");
        f.Correlations.FirstSendEnded(entry, boom);
        Assert.Null(entry.Request);
        f.Correlations.Reply(f.Reply(failedSend));
        Assert.Same(boom, (await f.Correlations.CompleteAsync(entry)).Error);
        Assert.Equal("1", f.Count("messages.lateReply"));

        var echoFirst = f.Request(2);
        var second = f.Correlations.Register(echoFirst, PerfClock.Now);
        f.Correlations.Reply(f.Reply(echoFirst));
        f.Correlations.FirstSendEnded(second, boom);
        Assert.Null((await f.Correlations.CompleteAsync(second)).Error);
        Assert.Equal("1", f.Count("messages.lateReply"));
    }

    [Fact]
    public async Task AReplyWithAWrongPayloadOrIdentityFailsTheOperationAndIsNotASuccess()
    {
        using var f = new Fixture();
        var request = f.Request(1);
        var entry = f.Correlations.Register(request, PerfClock.Now);
        f.Correlations.FirstSendEnded(entry, null);
        var bytes = Convert.FromBase64String(request.payload);
        bytes[^1] ^= 1;
        f.Correlations.Reply(f.Reply(request) with { payload = Convert.ToBase64String(bytes) });
        var (error, _) = await f.Correlations.CompleteAsync(entry);
        Assert.Equal("PayloadMismatch", Assert.IsType<PerfValidationException>(error).Kind);

        var other = f.Request(2);
        var second = f.Correlations.Register(other, PerfClock.Now);
        f.Correlations.Reply(f.Reply(other) with { sequence = "99" });
        Assert.Equal(
            "IdentityMismatch",
            Assert
                .IsType<PerfValidationException>((await f.Correlations.CompleteAsync(second)).Error)
                .Kind
        );
    }

    [Fact]
    public async Task AReplyForAnUnissuedCorrelationIsCountedAndChangesNoOperation()
    {
        using var f = new Fixture();
        var known = f.Request(1);
        var entry = f.Correlations.Register(known, PerfClock.Now);
        f.Correlations.Reply(f.Reply(f.Request(2)));
        Assert.Equal("1", f.Count("messages.unknownCorrelation"));
        // The stray reply closed nothing: the issued operation still runs into its own expiry.
        Assert.Equal(
            "CorrelationExpired",
            Assert
                .IsType<PerfValidationException>((await f.Correlations.CompleteAsync(entry)).Error)
                .Kind
        );
        Assert.Throws<PerfValidationException>(() => f.Correlations.Register(known, PerfClock.Now));
    }

    [Fact]
    public void AdmittedCountsOnlyANormalFirstSendInsideAnActiveCellNotTheSetupProbe()
    {
        using var f = new Fixture();
        var probe = f.Correlations.Register(f.Request(1), PerfClock.Now);
        f.Correlations.FirstSendEnded(probe, null); // Phase is "setup": a probe is not an admitted measured send
        Assert.Equal("0", f.Count("messages.admitted"));
    }

    [Fact]
    public async Task ScenarioMetricsClearAtResetAndKeepUnsupportedKeysNullWithTheirReason()
    {
        using var f = new Fixture();
        f.Metrics.Latency("driverLatencyMs", "driver.latency")
            .SpotInternalsUnsupported()
            .AliasLatency("latency", "spot.remoteCallLatency");
        f.Metrics.Count("driver.issued", 3);
        var before = f.Measurement.Snapshot(null);
        Assert.Equal("3", before.metrics["driver.issued"]);
        Assert.Null(before.metrics["spot.mailboxDepth.max"]);
        Assert.Equal(
            "PUBLIC_OBSERVATION_UNSUPPORTED",
            before.nullReasons["/metrics/spot.mailboxDepth.max"].code
        );
        Assert.Null(before.metrics["spot.remoteCallLatency.p50Ms"]);
        Assert.Equal(
            "NO_SAMPLES",
            before.nullReasons["/metrics/spot.remoteCallLatency.p50Ms"].code
        );
        Assert.Contains("driverLatencyMs", before.histograms.Keys);
        // A key the scenario never named keeps the reason Measurement gave it.
        Assert.Equal("NOT_APPLICABLE", before.nullReasons["/metrics/fanout.subscriberCount"].code);
        f.Measurement.Start(
            new PerfTriggerRequest
            {
                runId = "test",
                cellId = f.Measurement.Config.cellId,
                phase = "warmup",
                resetSeq = "0",
            },
            null
        );
        await f.Measurement.PhaseTask;
        Assert.True(
            f.Measurement.Reset(
                new ResetRequest
                {
                    runId = "test",
                    cellId = f.Measurement.Config.cellId,
                    resetSeq = "1",
                },
                null
            ).ok
        );
        Assert.Equal("0", f.Measurement.Snapshot(null).metrics["driver.issued"]);
    }
}
