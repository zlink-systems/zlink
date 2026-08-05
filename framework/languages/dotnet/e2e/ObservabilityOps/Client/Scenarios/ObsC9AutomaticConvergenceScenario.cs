// Verifies OBS-C9 requires automatic topology convergence before relocation.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC9AutomaticConvergenceScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var sourceNode = await context.PlayNodeIdAsync("play-a");
        var targetNode = await context.PlayNodeIdAsync("play-b");
        await using var workload = await context.PreparePlayWorkloadAsync(
            sourceNode, "c9-automatic");
        var relocation = (await context.PlayA.Post("/relocate/direct")
            .Body(new RelocateHostReq(
                "planned-maintenance", null, 30000))
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<RelocateHostRes>()).Body;
        ZlinkStreamAssert.Ensure(
            relocation is
            {
                Mode: "PlannedMaintenance",
                TargetApplicationVersion: 1,
                Outcome: "Relocated",
                Reason: "None"
            },
            $"OBS-C9 automatic relocation returned "
            + $"{relocation.Outcome}/{relocation.Reason}.");
        await context.WaitForPlayWorkloadAsync(
            workload.Room.RoomRid,
            workload.ActorId,
            targetNode,
            TimeSpan.FromSeconds(15));
        var action = await workload.Connector
            .Request(new GameActionReq("obs-c9-after-ready"))
            .Async<GameActionRes>();
        ZlinkStreamAssert.Ensure(
            action.NodeRid == targetNode,
            "OBS-C9 traffic did not continue on the admitted target.");

        var shutdown = (await context.PlayA.Post("/shutdown/direct")
            .Body(new ShutdownHostReq())
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<ShutdownHostRes>()).Body;
        ZlinkStreamAssert.Ensure(
            shutdown is { Outcome: "Stopped", Reason: "None" },
            $"OBS-C9 automatic shutdown returned "
            + $"{shutdown.Outcome}/{shutdown.Reason}.");
        Console.WriteLine("scenario OBS-C9A passed");
    }
}

internal static class ObsC9ManualTopologyScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var sourceNode = await context.PlayNodeIdAsync("play-a");
        await using var workload = await context.PreparePlayWorkloadAsync(
            sourceNode, "c9-manual");
        var before = (await context.PlayA.Get("/runtime/status")
            .Async<RuntimeStatusRes>()).Body;
        var relocation = (await context.PlayA.Post("/relocate/direct")
            .Body(new RelocateHostReq(
                "planned-maintenance", null, 30000))
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<RelocateHostRes>()).Body;
        var after = (await context.PlayA.Get("/runtime/status")
            .Async<RuntimeStatusRes>()).Body;
        ZlinkStreamAssert.Ensure(
            relocation is
            {
                Outcome: "Blocked",
                Reason: "ManualTopologyUnsupported"
            },
            $"OBS-C9 manual relocation returned "
            + $"{relocation.Outcome}/{relocation.Reason}.");
        ZlinkStreamAssert.Ensure(
            before.State == "Serving"
            && after.State == "Serving"
            && after.IsReady
            && after.AcceptingWork,
            "OBS-C9 manual blocker changed source readiness or admission.");
        var action = await workload.Connector
            .Request(new GameActionReq("obs-c9-manual-continuity"))
            .Async<GameActionRes>();
        ZlinkStreamAssert.Ensure(
            action.NodeRid == sourceNode,
            "OBS-C9 manual blocker changed workload authority.");

        var shutdown = (await context.PlayA.Post("/shutdown/direct")
            .Body(new ShutdownHostReq())
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<ShutdownHostRes>()).Body;
        ZlinkStreamAssert.Ensure(
            shutdown.Outcome is "Stopped" or "ForceStopped",
            "OBS-C9 manual topology incorrectly blocked Shutdown.");
        Console.WriteLine("scenario OBS-C9B passed");
    }
}
