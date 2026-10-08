using Xunit;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;

namespace ZLink.Framework.Perf.Tests;

public sealed class SpotDriverClassificationTests
{
    [Fact]
    public async Task DriverUsesHandlerEchoAsSuccessWithoutSecondValidation()
    {
        var workload = TestWorkload.Create(.02, .02, drainTimeoutMs: 200, setupTimeoutMs: 1_000);
        var config = new RoleConfig(
            "test",
            "s2s-spot-to-channel-request-echo/1024/test",
            new string('d', 64),
            "spot",
            0,
            "s2s-spot-to-channel-request-echo",
            "routemesh",
            "channel",
            "mesh",
            [],
            null,
            "",
            "",
            true,
            "ObjectClient",
            null,
            ["spot-0"],
            [],
            "SpotWide",
            workload,
            []
        );
        using var measurement = new Measurement(config, true);
        var metrics = new ScenarioMetrics(measurement)
            .Counters(
                "driver.issued",
                "driver.notStarted",
                "driver.failed",
                "spot.applicationHandlerEntries",
                "spot.applicationYieldCalls"
            )
            .Latency("driverLatencyMs", "driver.latency")
            .AliasLatency("latency", "spot.remoteCallLatency")
            .SpotInternalsUnsupported();
        var invalidReply = new PerfDriveReply(
            true,
            new PerfEchoReply
            {
                runId = "wrong",
                cellId = config.cellId,
                resetSeq = "0",
                phase = "warmup",
                clientId = 0,
                sequence = "1",
                correlationId = "wrong",
                receivedTicks = "0",
                clockDomainId = PerfClock.Domain,
                payload = measurement.Pattern.Base64,
            }
        );
        var scenario = new S2sSpotToChannelRequestEchoScenario(
            new FakeSpotClient(invalidReply),
            null!,
            measurement,
            null!,
            null!,
            metrics,
            new long[1]
        );

        Assert.True(
            measurement
                .Start(
                    new PerfTriggerRequest
                    {
                        runId = config.runId,
                        cellId = config.cellId,
                        phase = "warmup",
                        resetSeq = "0",
                    },
                    scenario.RunAsync
                )
                .accepted
        );
        await measurement.PhaseTask;
        var snapshot = measurement.Snapshot(null);

        Assert.True(ulong.Parse((string)snapshot.metrics["driver.issued"]!) > 0);
        Assert.Equal("0", snapshot.metrics["driver.failed"]);
        Assert.NotNull(snapshot.metrics["driver.latency.p50Ms"]);
        Assert.True(
            ulong.Parse(
                Assert.IsType<HistogramSnapshot>(snapshot.histograms["driverLatencyMs"]).count
            ) > 0
        );
    }

    [Fact]
    public async Task SendSendDriverLatencyEndsWhenThePublicDriverCallReturns()
    {
        var config = new RoleConfig(
            "test",
            "s2s-spot-to-channel-send-send-echo/1024/test",
            new string('e', 64),
            "spot",
            0,
            "s2s-spot-to-channel-send-send-echo",
            "routemesh",
            "channel",
            "mesh",
            [],
            null,
            "",
            "",
            true,
            "ObjectClient",
            null,
            ["spot-0"],
            [],
            "SpotWide",
            TestWorkload.Create(.05, .05),
            [],
            mode: "send-send"
        );
        using var measurement = new Measurement(config, true);
        var metrics = new ScenarioMetrics(measurement)
            .Counters(
                "driver.issued",
                "driver.notStarted",
                "driver.failed",
                "spot.applicationHandlerEntries"
            )
            .Latency("driverLatencyMs", "driver.latency")
            .SpotInternalsUnsupported();
        var correlations = new SendSendCorrelation(measurement, metrics);
        var spots = new CallbackSpotClient(async drive =>
        {
            Assert.True(measurement.BeginOperation(out var started, "send"));
            var entry = correlations.Register(drive.echo, started);
            correlations.FirstSendEnded(entry, null);
            correlations.Reply(PayloadPattern.Reply(drive.echo, PerfClock.Now));
            await Task.Delay(20);
            return new PerfDriveReply(true, null);
        });
        var scenario = new S2sSpotToChannelSendSendEchoScenario(
            spots,
            null!,
            measurement,
            null!,
            null!,
            metrics,
            correlations,
            new long[1]
        );
        Assert.True(
            measurement
                .Start(
                    new PerfTriggerRequest
                    {
                        runId = config.runId,
                        cellId = config.cellId,
                        phase = "warmup",
                        resetSeq = "0",
                    },
                    scenario.RunAsync
                )
                .accepted
        );
        await measurement.PhaseTask;
        var histogram = Assert.IsType<HistogramSnapshot>(
            measurement.Snapshot(null).histograms["driverLatencyMs"]
        );
        Assert.True(
            long.Parse(histogram.maxNs!) >= 10_000_000,
            "Driver latency ended at correlation close before the driver call returned."
        );
    }

    private sealed class CallbackSpotClient(Func<PerfDriveRequest, Task<PerfDriveReply>> callback)
        : IZLinkSpotClient
    {
        public IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message) =>
            throw new NotSupportedException();

        public IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId, TRequest request) =>
            new CallbackSpotRequestCall(() => callback((PerfDriveRequest)(object)request!));
    }

    private sealed class CallbackSpotRequestCall(Func<Task<PerfDriveReply>> callback)
        : IZLinkSpotRequestCall
    {
        public IZLinkSpotRequestCall Metadata(string key, string value) => this;

        public IZLinkSpotRequestCall Metadata(ZLinkMessageMetadata metadata) => this;

        public IZLinkSpotRequestCall InstanceSpot() => this;

        public IZLinkSpotRequestCall InstanceSpot(string instanceSpotType) => this;

        public IZLinkSpotRequestCall InMesh(string meshName) => this;

        public IZLinkSpotRequestCall Timeout(TimeSpan timeout) => this;

        public async ValueTask<TReply> Async<TReply>(
            CancellationToken cancellationToken = default
        ) => (TReply)(object)await callback();

        public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default) =>
            Async<TReply>(cancellationToken);
    }

    private sealed class FakeSpotClient(PerfDriveReply reply) : IZLinkSpotClient
    {
        public IZLinkSpotSendCall SendToSpot<TMessage>(string spotId, TMessage message) =>
            throw new NotSupportedException();

        public IZLinkSpotRequestCall RequestToSpot<TRequest>(string spotId, TRequest request) =>
            new FakeSpotRequestCall(reply);
    }

    private sealed class FakeSpotRequestCall(PerfDriveReply reply) : IZLinkSpotRequestCall
    {
        public IZLinkSpotRequestCall Metadata(string key, string value) => this;

        public IZLinkSpotRequestCall Metadata(ZLinkMessageMetadata metadata) => this;

        public IZLinkSpotRequestCall InstanceSpot() => this;

        public IZLinkSpotRequestCall InstanceSpot(string instanceSpotType) => this;

        public IZLinkSpotRequestCall InMesh(string meshName) => this;

        public IZLinkSpotRequestCall Timeout(TimeSpan timeout) => this;

        public ValueTask<TReply> Async<TReply>(CancellationToken cancellationToken = default) =>
            Reply<TReply>();

        public ValueTask<TReply> Yield<TReply>(CancellationToken cancellationToken = default) =>
            Reply<TReply>();

        private ValueTask<TReply> Reply<TReply>()
        {
            if (typeof(TReply) != typeof(PerfDriveReply))
                throw new NotSupportedException(typeof(TReply).FullName);
            return ValueTask.FromResult((TReply)(object)reply);
        }
    }
}
