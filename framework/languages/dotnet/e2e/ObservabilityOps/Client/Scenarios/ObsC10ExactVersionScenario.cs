// Verifies OBS-C10 selects only the requested higher application version.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC10ExactVersionScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var playA = await context.PlayNodeIdAsync("play-a");
        var playB = await context.PlayNodeIdAsync("play-b");
        var playC = await context.PlayNodeIdAsync("play-c");
        var playD = await context.PlayNodeIdAsync("play-d");
        await using var planned = await context.PreparePlayWorkloadAsync(
            playA, "c10-planned");
        var plannedResult = (await context.PlayA.Post("/relocate/direct")
            .Body(new RelocateHostReq(
                "planned-maintenance", null, 30000))
            .Async<RelocateHostRes>()).Body;
        await context.WaitForPlayWorkloadAsync(
            planned.Room.RoomRid,
            planned.ActorId,
            playB,
            TimeSpan.FromSeconds(15));
        ZlinkStreamAssert.Ensure(
            plannedResult is
            {
                Mode: "PlannedMaintenance",
                TargetApplicationVersion: 1,
                Outcome: "Relocated",
                Reason: "None"
            },
            "OBS-C10 planned maintenance did not select exact version 1.");

        await using var rolling = await context.PreparePlayWorkloadAsync(
            playB, "c10-rolling");
        var rollingResult = (await context.PlayB.Post("/relocate/direct")
            .Body(new RelocateHostReq("rolling-update", 2, 30000))
            .Async<RelocateHostRes>()).Body;
        await context.WaitForPlayWorkloadAsync(
            rolling.Room.RoomRid,
            rolling.ActorId,
            playC,
            TimeSpan.FromSeconds(15));
        ZlinkStreamAssert.Ensure(
            rollingResult is
            {
                Mode: "RollingUpdate",
                TargetApplicationVersion: 2,
                Outcome: "Relocated",
                Reason: "None"
            },
            "OBS-C10 rolling update selected a version other than 2.");
        var evidence = (await context.PlayB.Get("/evidence")
            .Query("spotRid", rolling.Room.RoomRid)
            .Async<EvidenceSnapshot>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.SpotRows.All(row => row.NodeRid != playD),
            "OBS-C10 selected higher-weight version 3 before exact filtering.");
        Console.WriteLine("scenario OBS-C10 passed");
    }
}
