// Verifies OBS-C7 relocates planned maintenance to the same version.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC7PlannedMaintenanceScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var sourceNode = await context.PlayNodeIdAsync("play-a");
        var targetNode = await context.PlayNodeIdAsync("play-b");
        await using var workload = await context.PreparePlayWorkloadAsync(
            sourceNode, "c7");
        var result = (await context.PlayA.Post("/relocate/direct")
            .Body(new RelocateHostReq(
                "planned-maintenance", null, 30000))
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<RelocateHostRes>()).Body;
        ZlinkStreamAssert.Ensure(
            result is
            {
                Mode: "PlannedMaintenance",
                TargetApplicationVersion: 1,
                Outcome: "Relocated",
                Reason: "None"
            },
            $"OBS-C7 maintenance returned "
            + $"{result.Mode}/{result.TargetApplicationVersion}/"
            + $"{result.Outcome}/{result.Reason}.");

        await context.WaitForPlayWorkloadAsync(
            workload.Room.RoomRid,
            workload.ActorId,
            targetNode,
            TimeSpan.FromSeconds(15));
        var action = await workload.Connector.Request(
                new GameActionReq("obs-c7-after"))
            .Async<GameActionRes>();
        ZlinkStreamAssert.Ensure(
            action.NodeRid == targetNode,
            "OBS-C7 same-version target did not continue service.");

        var shutdown = (await context.PlayA.Post("/shutdown/direct")
            .Body(new ShutdownHostReq())
            .Async<ShutdownHostRes>()).Body;
        ZlinkStreamAssert.Ensure(
            shutdown is { Outcome: "Stopped", Reason: "None" },
            $"OBS-C7 shutdown returned "
            + $"{shutdown.Outcome}/{shutdown.Reason}.");
        Console.WriteLine("scenario OBS-C7 passed");
    }
}
