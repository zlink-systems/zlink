// Verifies OBS-B2 Actor Transfer Metrics behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsB2QueueAndTransferMetricsScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"obs-b2-{suffix}";
        // Relocation is coordinated by the source node, so its metrics are
        // recorded there. play-b runs with metrics disabled on purpose for
        // OBS-B4, so the measured move has to start on play-a.
        var source = await context.CreateRoomOnObservedNodeAsync(
            "play-a", $"room-b2-source-{suffix}");
        var target = await context.CreateRoomOnObservedNodeAsync(
            "play-b", $"room-b2-target-{suffix}");
        await using var connector = await context.ConnectAsync();
        await connector.Request(new AuthenticateReq(actorId)).Async<AuthenticateRes>();
        await context.JoinRoomAsync(connector, actorId, source.RoomRid);

        var moved = await context.JoinRoomAsync(connector, actorId, target.RoomRid);
        ZlinkStreamAssert.Ensure(
            moved.NodeRid == target.NodeRid,
            "OBS-B2 Actor relocation did not commit to the observed target.");
        var transfers = await WaitMetricAsync(
            context,
            "zlink.relocation.completed",
            1,
            new Dictionary<string, string>
            {
                ["object_kind"] = "actor",
                ["outcome"] = "completed"
            });
        var transferDuration = await WaitMetricAsync(
            context,
            "zlink.relocation.duration",
            0,
            new Dictionary<string, string> { ["object_kind"] = "actor" });
        var interruption = await WaitMetricAsync(
            context,
            "zlink.relocation.interruption",
            0,
            new Dictionary<string, string> { ["unit_kind"] = "actor" });

        ZlinkStreamAssert.Ensure(transfers.Any(sample => sample.Value >= 1),
            "OBS-B2 relocation completion counter did not increase.");
        ZlinkStreamAssert.Ensure(transferDuration.Any(sample => sample.Count > 0),
            "OBS-B2 relocation duration histogram was not recorded.");
        ZlinkStreamAssert.Ensure(interruption.Any(sample => sample.Count > 0),
            "OBS-B2 service interruption histogram was not recorded.");
        await connector.Close.Async();
        Console.WriteLine("scenario OBS-B2 passed");
    }

    private static async Task<MetricSample[]> WaitMetricAsync(
        ScenarioContext context,
        string name,
        decimal minimum,
        IReadOnlyDictionary<string, string>? tags = null) =>
        (await context.PlayA.Post("/metrics/wait")
            .Body(new MetricWaitReq(name, minimum, RequiredTags: tags))
            .Async<MetricSample[]>()).Body;
}
