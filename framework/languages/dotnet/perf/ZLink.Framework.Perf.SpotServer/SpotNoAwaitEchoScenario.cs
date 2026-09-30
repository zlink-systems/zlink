using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// §10.7 spot-no-await-echo (also the §11.3 local Spot reference). Question: what the public Spot client costs from
// the caller to a User Spot in the same process that echoes at once; not a pure mailbox cost. Roles: HTTP Client x1,
// Spot process (Object Server + local application driver, this file) x1; no Channel, Actor or worker. One operation:
// the local IZLinkSpotClient.RequestToSpot until the typed echo is validated (codec and local dispatch included,
// the HTTP trigger is not). no-await: the caller is an ordinary request and the handler replies with the typed echo
// at once. Payload 1024 bytes. Setup: only this Object Server can place the Spots. Store: run Docker Redis (Spot
// addresses). Null: remote call, worker, Actor, fanout; mailbox depth and real turns have no public observation;
// the driver histogram is not kept because this interval is already the primary latency.
public sealed class SpotNoAwaitEchoScenario(IZLinkSpotClient spots, IZLinkSpotManager manager, Measurement measurement,
    IZLinkRouteMeshRuntime meshRuntime, ObjectsReadiness readiness)
{
    private readonly RoleConfig config = measurement.Config;
    private long[] sequences = [];

    public static async Task RunAsync(RoleConfig config)
    {
        if (config.role != "spot" || !config.source) throw new ArgumentException("The Spot role is the source of this scenario.");
        var builder = SpotRole.Builder<PerfEchoSpot>(config, callsChannel: false);
        builder.Services.AddSingleton(sp => new ScenarioMetrics(sp.GetRequiredService<Measurement>())
            .Counters("spot.applicationHandlerEntries").SpotInternalsUnsupported());
        builder.Services.AddSingleton<SpotNoAwaitEchoScenario>();
        var app = builder.Build();
        _ = app.Services.GetRequiredService<ScenarioMetrics>();
        var scenario = app.Services.GetRequiredService<SpotNoAwaitEchoScenario>();
        ServerApplication.Map(app, scenario.RunAsync);
        await app.StartAsync();
        await scenario.PrepareAsync(app.Lifetime.ApplicationStopping);
        await app.WaitForShutdownAsync();
    }

    public async Task PrepareAsync(CancellationToken stopping)
    {
        var objects = await SpotRole.CreateSpotsAsync(config, manager, meshRuntime, measurement, stopping);
        if (objects is null) return;
        try
        {
            sequences = new long[config.workload.logicalStreams!.Value];
            List<object> probes = [];
            for (var target = 0; target < config.spotIds.Length; target++)
            {
                var request = measurement.Request(target, (ulong)Interlocked.Increment(ref sequences[target % sequences.Length]), probe: true);
                var reply = await spots.RequestToSpot(config.spotIds[target], request)
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs)).Async<PerfEchoReply>();
                PayloadPattern.ValidateIdentity(request, reply);
                measurement.Pattern.Validate(reply.payload);
                probes.Add(new { request.correlationId, reply.receivedTicks, reply.clockDomainId });
            }
            SpotRole.Publish(readiness, objects);   // objectsReady only after every probe, so warmup never overlaps one
            measurement.SetupEvidence = [new { kind = "typedProbeEcho", source = "IZLinkSpotClient.RequestToSpot.Async<PerfEchoReply>", observedValue = probes }];
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
            var request = measurement.Request(stream, checked((ulong)Interlocked.Increment(ref sequences[stream])));
            if (!measurement.BeginOperation(out var started)) break;
            request = request with { sentTicks = DecimalText.Of(started) };
            try
            {
                var reply = await spots.RequestToSpot(spotId, request)
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs)).Async<PerfEchoReply>();
                PayloadPattern.ValidateIdentity(request, reply);
                measurement.Pattern.Validate(reply.payload);
                measurement.CompleteOperation(started);
            }
            catch (Exception error) { measurement.CompleteOperation(started, error); }
        }
    }
}
