using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// §10.6 s2s-spot-to-channel-send-send-echo. Question: what a send from a Spot to a Channel that comes back to the
// original Spot as a separate send costs in completion rate and time. Roles: HTTP Client x1, Spot process (Object
// Server + local public driver, this file) x1, Channel target (Object Client) x1. The driver sends PerfDriveRequest
// to the Spot with IZLinkSpotClient.RequestToSpot; the Spot handler registers the correlation, makes the first
// SendToChannel and returns once that send is admitted, so the turn is free when the Channel's send comes back to
// the Spot's return handler. The driver, outside the turn, waits for the correlation and keeps the in-flight slot
// until the echo is validated (§13). One operation: correlation registration / first send -> return handler echo
// validation. The DTO's returnSpotId names the source User SpotId. send-send; ordinary; payload 4096 bytes.
// Store: run Docker Redis. Null: physical connections, worker, Actor, fanout; Spot internals are not observable.
public sealed class S2sSpotToChannelSendSendEchoScenario(IZLinkSpotClient spots, IZLinkSpotManager manager, Measurement measurement,
    IZLinkRouteMeshRuntime meshRuntime, ObjectsReadiness readiness, ScenarioMetrics metrics, SendSendCorrelation correlations)
{
    private readonly RoleConfig config = measurement.Config;
    private long[] sequences = [];

    public static async Task RunAsync(RoleConfig config)
    {
        if (config.role != "spot" || !config.source) throw new ArgumentException("The Spot role is the source of this scenario.");
        var builder = SpotRole.Builder<S2sSendSendSpot>(config, callsChannel: true);
        builder.Services.AddSingleton(sp => new ScenarioMetrics(sp.GetRequiredService<Measurement>())
            .Counters("driver.issued", "driver.notStarted", "driver.failed", "spot.applicationHandlerEntries")
            .Latency("driverLatencyMs", "driver.latency").SpotInternalsUnsupported());
        builder.Services.AddSingleton<SendSendCorrelation>();
        builder.Services.AddSingleton<S2sSpotToChannelSendSendEchoScenario>();
        var app = builder.Build();
        var scenario = app.Services.GetRequiredService<S2sSpotToChannelSendSendEchoScenario>();
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
                var echo = measurement.Request(target, (ulong)Interlocked.Increment(ref sequences[target % sequences.Length]), probe: true)
                    with { returnSpotId = config.spotIds[target] };
                var driven = await spots.RequestToSpot(config.spotIds[target], new PerfDriveRequest(echo))
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs * 2)).Async<PerfDriveReply>();
                if (!driven.started) throw new PerfValidationException("IdentityMismatch", "The setup probe was not started.");
                var (error, _) = await correlations.CompleteAsync(correlations.Find(echo.correlationId)
                    ?? throw new PerfValidationException("UnknownCorrelation", "The setup probe registered no correlation."));
                if (error is not null) throw error;
                probes.Add(new { echo.correlationId });
            }
            SpotRole.Publish(readiness, objects);   // objectsReady only after every probe, so warmup never overlaps one
            measurement.SetupEvidence = [new { kind = "typedProbeEcho", source = "Spot SendToChannel -> Channel SendToSpot -> Spot return handler", observedValue = probes }];
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); }
    }

    public Task RunAsync() => Task.WhenAll(Enumerable.Range(0, config.workload.logicalStreams!.Value)
        .SelectMany(stream => Enumerable.Range(0, config.workload.inflight).Select(_ => LoopAsync(stream))));

    private async Task LoopAsync(int stream)
    {
        var spotId = config.spotIds[stream % config.spotIds.Length];
        while (measurement.CanIssue)
        {
            var echo = measurement.Request(stream, checked((ulong)Interlocked.Increment(ref sequences[stream]))) with { returnSpotId = spotId };
            var driverStarted = PerfClock.Now;
            metrics.Count("driver.issued");
            try
            {
                var driven = await spots.RequestToSpot(spotId, new PerfDriveRequest(echo))
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs * 2)).Async<PerfDriveReply>();
                if (!driven.started) { metrics.Count("driver.notStarted"); continue; }
                // Outside the Spot turn: the final result of the correlation the handler registered (§13).
                var entry = correlations.Find(echo.correlationId) ?? throw new PerfValidationException("UnknownCorrelation", "The started drive registered no correlation.");
                var (result, completed) = await correlations.CompleteAsync(entry);
                measurement.CompleteOperation(entry.StartedTicks, result, completed);
                if (result is null) metrics.Record("driverLatencyMs", driverStarted, PerfClock.Now);
            }
            catch (Exception error)
            {
                metrics.Count("driver.failed");
                measurement.RecordDiagnostic(error);
            }
        }
    }
}

public sealed class S2sSendSendSpot(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;
    public void Configure()
    {
        Context.Handlers.AddPacket<S2sSendDriveHandler>();
        Context.Handlers.AddPacket<S2sSendReturnHandler>();
    }
}

// The source Spot handler: register the correlation, make the first public send and return when it is admitted.
public sealed class S2sSendDriveHandler(Measurement measurement, ScenarioMetrics metrics, SendSendCorrelation correlations, RoleConfig config)
    : IZLinkSpotRequestHandler<S2sSendSendSpot, PerfDriveRequest, PerfDriveReply>
{
    public async ValueTask<PerfDriveReply> HandleAsync(S2sSendSendSpot spot, PerfDriveRequest drive, CancellationToken cancellationToken)
    {
        measurement.HandlerEnter();
        try
        {
            var request = drive.echo;
            measurement.ValidateRequest(request, returnSpotId: request.returnSpotId);
            if (string.IsNullOrEmpty(request.returnSpotId)) throw new PerfValidationException("IdentityMismatch", "No return SpotId in the request.");
            if (request.phase == "measured") metrics.Count("spot.applicationHandlerEntries");
            var probe = measurement.Phase == "setup";   // the setup probe is no measured operation
            var started = PerfClock.Now;
            if (!probe && !measurement.BeginOperation(out started, "send")) return new PerfDriveReply(false, null);
            request = request with { sentTicks = DecimalText.Of(started) };
            try
            {
                var entry = correlations.Register(request, started);   // §13: registered right before the first public send
                try
                {
                    await spot.Context.Outbound.SendToChannel(config.channelName!, request).Async(cancellationToken);
                    correlations.FirstSendEnded(entry, null);
                }
                catch (Exception error) { correlations.FirstSendEnded(entry, error); }
            }
            catch (Exception error)
            {
                // The operation started but cannot be tied to a correlation: it ends here as a failure.
                if (!probe) measurement.CompleteOperation(started, error);
                throw;
            }
            return new PerfDriveReply(true, null);   // send/send: the reply is only the first send's acknowledgement
        }
        finally { measurement.HandlerExit(); }
    }
}

// The return send arrives as its own Spot packet: the correlation decides the operation's first result.
public sealed class S2sSendReturnHandler(Measurement measurement, ScenarioMetrics metrics, SendSendCorrelation correlations)
    : IZLinkSpotPacketHandler<S2sSendSendSpot, PerfEchoReply>
{
    public ValueTask HandleAsync(S2sSendSendSpot spot, PerfEchoReply message, CancellationToken cancellationToken)
    {
        measurement.HandlerEnter();
        try
        {
            if (message.phase == "measured") metrics.Count("spot.applicationHandlerEntries");
            correlations.Reply(message);
            return ValueTask.CompletedTask;
        }
        finally { measurement.HandlerExit(); }
    }
}
