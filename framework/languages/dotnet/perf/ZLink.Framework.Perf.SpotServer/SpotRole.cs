using Microsoft.AspNetCore.Builder;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// What every Spot role of §10.3-§10.8 shares: an automatic RouteMesh Object Server hosting this cell's User Spots as
// Actor-less SpotWide Spots (§10), and their creation through the public manager as setup. No call that a scenario
// measures lives here; each scenario file shows its own requests, sends and Yield/worker calls.
public static class SpotRole
{
    public const string SpotType = "perf-spot";

    // callsChannel: the Spot handler calls this cell's ChannelName (§10.5, §10.6), so the node registers a Channel client.
    public static WebApplicationBuilder Builder<TSpot>(RoleConfig config, bool callsChannel, Action<IZLinkFrameworkOptions>? more = null)
        where TSpot : class, IZLinkSpot
    {
        var builder = ServerApplication.Builder(config, options =>
        {
            more?.Invoke(options);
            var mesh = options.AddRouteMesh(config.meshName!).SetRoutingIdPrefix("perf-spot").Listen(config.transportEndpoints["mesh"]);
            if (callsChannel) mesh.Channel(config.channelName!).Client();
            mesh.Objects().Server().AddSpotFactory<TSpot>(SpotType, factory => factory
                .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide).StableTypeLimit(config.spotIds.Length).DisableRelocation());
        });
        builder.Services.AddSingleton(new ObjectsReadiness(false, "This cell has not created its User Spots yet."));
        return builder;
    }

    // The public create results of this cell (§16.1 objectsReady): the manager result and the mesh placement count.
    public sealed record SpotObjects(bool Ready, object[] Evidence);

    // Setup: every SpotId of the cell is created through the public manager. Not part of any measured latency.
    // A role that probes reports objectsReady only after its probes, so warmup never overlaps a probe.
    public static async Task<SpotObjects?> CreateSpotsAsync(RoleConfig config, IZLinkSpotManager manager, IZLinkRouteMeshRuntime mesh,
        Measurement measurement, CancellationToken stopping)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        timeout.CancelAfter(config.workload.setupTimeoutMs);
        try
        {
            List<object> created = [];
            foreach (var spotId in config.spotIds)
            {
                var result = await manager.GetOrCreate(spotId, SpotType)
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.setupTimeoutMs)).Async(timeout.Token);
                if (result.State == ZLinkSpotCreateState.Rejected) throw new InvalidOperationException($"Spot {spotId} was rejected.");
                created.Add(new { spotId, state = result.State.ToString(), result.Spot.MeshName });
            }
            var placement = mesh.GetStatus(config.meshName!).Placement;
            return new SpotObjects(placement.IsAvailable && placement.ActiveSpotCount >= config.spotIds.Length,
                [new { kind = "spotCreate", source = "IZLinkSpotManager.GetOrCreate.Async", observedValue = created },
                 new { kind = "spotPlacement", source = "IZLinkRouteMeshRuntime.GetStatus.Placement",
                    observedValue = new { placement.IsAvailable, placement.ActiveSpotCount, expectedSpots = config.spotIds.Length } }]);
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); return null; }
    }

    public static void Publish(ObjectsReadiness readiness, SpotObjects objects) =>
        readiness.Set(objects.Ready, "The Object Server has not activated every User Spot of this cell.", objects.Evidence);
}
