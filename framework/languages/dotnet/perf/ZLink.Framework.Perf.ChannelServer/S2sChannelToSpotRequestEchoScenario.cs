using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// §10.3 s2s-channel-to-spot-request-echo. Question: what a remote request to a global SpotId costs end to end
// (address lookup, delivery, reply). Roles: HTTP trigger Client x1, Channel process (Object Client, this file) x1,
// Spot process (Object Server, SpotServer) x1. One operation starts at the Channel's RequestToSpot and ends when the
// typed echo is validated. request, ordinary; representative payload 4096 bytes. streamId mod spotCount picks the
// User Spot. Store: run Docker Redis (automatic discovery, Spot addresses). Null: physical connections, worker,
// Actor, fanout; Spot mailbox and turn internals have no public observation.
public sealed class S2sChannelToSpotRequestEchoScenario(IZLinkSpotClient spots, IZLinkSpotManager manager, Measurement measurement,
    IZLinkRouteMeshRuntime meshRuntime, ObjectsReadiness readiness)
{
    private readonly RoleConfig config = measurement.Config;
    private readonly ScenarioMetrics metrics = new ScenarioMetrics(measurement).SpotInternalsUnsupported();
    private long[] sequences = [];

    public static async Task RunAsync(RoleConfig config)
    {
        if (config.role != "channel" || !config.source) throw new ArgumentException("The Channel role is the source of this scenario.");
        var builder = ServerApplication.Builder(config, options =>
            options.AddRouteMesh(config.meshName!).SetRoutingIdPrefix("perf-channel").Listen(config.transportEndpoints["mesh"])
                .Objects().Client());
        builder.Services.AddSingleton(new ObjectsReadiness(false, "No User Spot has been found through the public manager yet."));
        builder.Services.AddSingleton<S2sChannelToSpotRequestEchoScenario>();
        var app = builder.Build();
        var scenario = app.Services.GetRequiredService<S2sChannelToSpotRequestEchoScenario>();
        ServerApplication.Map(app, scenario.RunAsync);
        await app.StartAsync();
        await scenario.PrepareAsync(app.Lifetime.ApplicationStopping);
        await app.WaitForShutdownAsync();
    }

    public async Task PrepareAsync(CancellationToken stopping)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        timeout.CancelAfter(config.workload.setupTimeoutMs);
        try
        {
            // Public status and the manager's resolve are polled; the probe call itself is never retried.
            while (meshRuntime.GetStatus(config.meshName!) is not { IsReady: true, ReadyPeerCount: > 0 }) await Task.Delay(10, timeout.Token);
            List<object> found = [];
            foreach (var spotId in config.spotIds)
            {
                SpotRef? spot;
                while ((spot = await manager.FindAsync(spotId, timeout.Token)) is null) await Task.Delay(10, timeout.Token);
                found.Add(new { spotId, spot.Value.MeshName, nodeRid = spot.Value.NodeRid.ToHex() });
            }
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
            readiness.Set(true, "", [new { kind = "spotFind", source = "IZLinkSpotManager.FindAsync", observedValue = found }]);   // only after every probe
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
