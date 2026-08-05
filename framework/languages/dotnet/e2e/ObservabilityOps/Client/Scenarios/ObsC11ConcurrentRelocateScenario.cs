// Verifies OBS-C11 joins equal Relocate calls and rejects conflicting intent.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC11ConcurrentRelocateScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var sourceNode = await context.PlayNodeIdAsync("play-a");
        await using var workload = await context.PreparePlayWorkloadAsync(
            sourceNode, "c11");

        var intent = new RelocateHostReq("rolling-update", 2, 30000);
        var first = context.PlayA.Post("/relocate/direct")
            .Body(intent).Timeout(TimeSpan.FromSeconds(35))
            .Async<RelocateHostRes>().AsTask();
        await WaitForServingPreflightAsync(context, first);
        var joined = context.PlayA.Post("/relocate/direct")
            .Body(intent).Timeout(TimeSpan.FromSeconds(35))
            .Async<RelocateHostRes>().AsTask();
        var conflictingMode = (await context.PlayA.Post("/relocate/direct")
            .Body(new RelocateHostReq(
                "planned-maintenance", null, 30000))
            .Async<RelocateHostRes>()).Body;
        var conflictingVersion = (await context.PlayA.Post("/relocate/direct")
            .Body(new RelocateHostReq("rolling-update", 3, 30000))
            .Async<RelocateHostRes>()).Body;
        ZlinkStreamAssert.Ensure(
            conflictingMode is
            {
                Outcome: "Blocked",
                Reason: "OperationInProgress"
            }
            && conflictingVersion is
            {
                Outcome: "Blocked",
                Reason: "OperationInProgress"
            },
            "OBS-C11 conflicting relocation options changed the operation.");

        File.WriteAllText(
            Path.Combine(context.Options.LogDir, "obs-c11-start-target"),
            "ready");
        var firstResult = (await first).Body;
        var joinedResult = (await joined).Body;
        ZlinkStreamAssert.Ensure(
            firstResult == joinedResult
            && firstResult is
            {
                Outcome: "Relocated",
                Reason: "None",
                TargetApplicationVersion: 2
            },
            "OBS-C11 same intent did not share one Relocated result: "
            + $"first={firstResult}, joined={joinedResult}.");
        var timerEvidence = await context.WaitPlayBEvidenceAsync(
            $"timer-tick|room={workload.Room.RoomRid}");
        ZlinkStreamAssert.Ensure(
            timerEvidence.Any(line => line.Contains(
                $"timer-tick|room={workload.Room.RoomRid}",
                StringComparison.Ordinal)),
            "OBS-C11 did not restore the User SPOT logical timer on the target.");
        var nextIntent = (await context.PlayA.Post("/relocate/direct")
            .Body(new RelocateHostReq(
                "planned-maintenance", null, 500))
            .Async<RelocateHostRes>()).Body;
        ZlinkStreamAssert.Ensure(
            nextIntent.Mode == "PlannedMaintenance"
            && nextIntent.Reason != "OperationInProgress",
            "OBS-C11 retained the completed operation as the active intent.");
        Console.WriteLine("scenario OBS-C11 passed");
    }

    private static async Task WaitForServingPreflightAsync(
        ScenarioContext context,
        Task first)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var status = (await context.PlayA.Get("/runtime/status")
                .Async<RuntimeStatusRes>()).Body;
            if (status.State == "Serving" && !first.IsCompleted) return;
            await Task.Delay(50);
        }

        throw new TimeoutException(
            "OBS-C11 relocation did not wait in Serving preflight.");
    }
}
