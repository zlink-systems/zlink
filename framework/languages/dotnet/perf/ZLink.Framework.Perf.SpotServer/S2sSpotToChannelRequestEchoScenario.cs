using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// §10.5 s2s-spot-to-channel-request-echo. Question: how the terminal (ordinary or Yield) and the number of Spots
// change completion throughput, tail latency and the progress of other callbacks for the same Spot -> Channel remote
// request. Roles: HTTP Client x1, Spot process (Object Server + local public driver, this file) x1, Channel echo
// target x1. The driver sends PerfDriveRequest to the Spot with IZLinkSpotClient.RequestToSpot; the Spot handler
// (an Actor-less SpotWide User Spot) makes one RequestToChannel. The measured operation starts right before that
// remote call and ends after the reply is validated (after the turn is regained); driver time is kept apart as
// driver.latency.*. Streams are assigned to SpotIds round-robin, so no Actor queue takes part.
// request; ordinary or yield x 1 or 16 Spots; payload 4096 bytes. Store: run Docker Redis (Spot addresses and
// automatic mesh). Null: physical connections, worker, Actor, fanout; suspended/resumed turns, resume latency and
// mailbox depth have no public observation. Yield calls are the application's calls, not proven turn suspensions.
public sealed class S2sSpotToChannelRequestEchoScenario(IZLinkSpotClient spots, IZLinkSpotManager manager, Measurement measurement,
    IZLinkRouteMeshRuntime meshRuntime, ObjectsReadiness readiness, ScenarioMetrics metrics, long[]? initialSequences = null)
{
    private readonly RoleConfig config = measurement.Config;
    private long[] sequences = initialSequences ?? [];

    public static async Task RunAsync(RoleConfig config)
    {
        if (config.role != "spot" || !config.source) throw new ArgumentException("The Spot role is the source of this scenario.");
        var builder = SpotRole.Builder<S2sRemoteRequestSpot>(config, callsChannel: true);
        builder.Services.AddSingleton(sp => new ScenarioMetrics(sp.GetRequiredService<Measurement>())
            .Counters("driver.issued", "driver.notStarted", "driver.failed", "spot.applicationHandlerEntries", "spot.applicationYieldCalls")
            .Latency("driverLatencyMs", "driver.latency").AliasLatency("latency", "spot.remoteCallLatency").SpotInternalsUnsupported());
        builder.Services.AddSingleton<S2sSpotToChannelRequestEchoScenario>();
        var app = builder.Build();
        var scenario = app.Services.GetRequiredService<S2sSpotToChannelRequestEchoScenario>();
        ServerApplication.Map(app, scenario.RunAsync);
        await app.StartAsync();
        await scenario.PrepareAsync(app.Lifetime.ApplicationStopping);
        await app.WaitForShutdownAsync();
    }

    public async Task PrepareAsync(CancellationToken stopping)
    {
        var objects = await SpotRole.CreateSpotsAsync(config, manager, meshRuntime, measurement, stopping);
        if (objects is null) return;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        timeout.CancelAfter(config.workload.setupTimeoutMs);
        try
        {
            while (!meshRuntime.GetStatus(config.meshName!).Channels.Any(c => c.ChannelName == config.channelName && c.IsReady && c.ReadyTargetCount > 0))
                await Task.Delay(10, timeout.Token);
            sequences = new long[config.workload.logicalStreams!.Value];
            List<object> probes = [];
            for (var target = 0; target < config.spotIds.Length; target++)
            {
                var echo = measurement.Request(target, (ulong)Interlocked.Increment(ref sequences[target % sequences.Length]), probe: true);
                var driven = await spots.RequestToSpot(config.spotIds[target], new PerfDriveRequest(echo))
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.driverTimeoutMs)).Async<PerfDriveReply>();
                if (!driven.started || driven.echo is null) throw new PerfValidationException("IdentityMismatch", "The setup probe did not reach the Channel.");
                PayloadPattern.ValidateIdentity(echo, driven.echo);
                measurement.Pattern.Validate(driven.echo.payload);
                probes.Add(new { echo.correlationId, driven.echo.receivedTicks, driven.echo.clockDomainId });
            }
            SpotRole.Publish(readiness, objects);   // objectsReady only after every probe, so warmup never overlaps one
            measurement.SetupEvidence = [new { kind = "typedProbeEcho", source = "IZLinkSpotClient.RequestToSpot -> Spot RequestToChannel", observedValue = probes }];
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); }
    }

    public Task RunAsync() => Task.WhenAll(Enumerable.Range(0, config.workload.logicalStreams!.Value)
        .SelectMany(stream => Enumerable.Range(0, config.workload.inflight).Select(_ => LoopAsync(stream))));

    // The local driver: one PerfDriveRequest per operation; the in-flight slot is the driver's until the handler returns.
    private async Task LoopAsync(int stream)
    {
        var spotId = config.spotIds[stream % config.spotIds.Length];
        while (measurement.CanIssue)
        {
            var echo = measurement.Request(stream, checked((ulong)Interlocked.Increment(ref sequences[stream])));
            var drive = new PerfDriveRequest(echo);
            var started = PerfClock.Now;
            metrics.Count("driver.issued");
            PerfDriveReply driven;
            try
            {
                driven = await spots.RequestToSpot(spotId, drive)
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.driverTimeoutMs)).Async<PerfDriveReply>();
            }
            catch (Exception error)
            {
                metrics.Count("driver.failed");
                measurement.RecordDiagnostic(error);
                continue;
            }
            if (!driven.started) metrics.Count("driver.notStarted");
            else if (driven.echo is not null) metrics.Record("driverLatencyMs", started, PerfClock.Now);
        }
    }
}

public sealed class S2sRemoteRequestSpot(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;
    public void Configure() => Context.Handlers.AddPacket<S2sRemoteRequestDriveHandler>();
}

// The Spot handler: one RequestToChannel per drive request. Ordinary keeps the Spot turn until the reply; Yield hands
// the turn back while the reply is pending (execution gate contract). This is the measured operation.
public sealed class S2sRemoteRequestDriveHandler(Measurement measurement, ScenarioMetrics metrics, RoleConfig config)
    : IZLinkSpotRequestHandler<S2sRemoteRequestSpot, PerfDriveRequest, PerfDriveReply>
{
    public async ValueTask<PerfDriveReply> HandleAsync(S2sRemoteRequestSpot spot, PerfDriveRequest drive, CancellationToken cancellationToken)
    {
        measurement.HandlerEnter();
        try
        {
            var request = drive.echo;
            measurement.ValidateRequest(request);
            if (request.phase == "measured") metrics.Count("spot.applicationHandlerEntries");
            var probe = measurement.Phase == "setup";   // the setup probe is no measured operation
            var started = PerfClock.Now;
            if (!probe && !measurement.BeginOperation(out started)) return new PerfDriveReply(false, null);
            request = request with { sentTicks = DecimalText.Of(started) };
            try
            {
                var call = spot.Context.Outbound.RequestToChannel(config.channelName!, request)
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs));
                PerfEchoReply reply;
                if (config.terminal == "yield")
                {
                    metrics.Count("spot.applicationYieldCalls");
                    reply = await call.Yield<PerfEchoReply>(cancellationToken);
                }
                else reply = await call.Async<PerfEchoReply>(cancellationToken);
                PayloadPattern.ValidateIdentity(request, reply);
                measurement.Pattern.Validate(reply.payload);
                if (!probe) measurement.CompleteOperation(started);
                return new PerfDriveReply(true, reply);
            }
            catch (Exception error)
            {
                if (probe) throw;
                measurement.CompleteOperation(started, error);
                return new PerfDriveReply(true, null);
            }
        }
        finally { measurement.HandlerExit(); }
    }
}
