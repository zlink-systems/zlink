// Verifies SM-E3 Idle Timer Spot Close behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmE3IdleTimerSpotCloseScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var spotRid = $"spot-sm-e3-{Guid.NewGuid():N}";
        var created = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
            "SM-E3 idle spot was not created on play-a.");
        var idle = (await playA.Post("/spot/idle-close/start")
            .Body(new SpotIdleCloseReq(spotRid, "sm-e3-idle", 50))
            .Async<SpotIdleCloseRes>()).Body;
        ZlinkStreamAssert.Ensure(idle.Closed, "SM-E3 idle close did not close the spot.");
        var closedSpotRequest = (await playA.Post("/spot/missing-target/request")
            .Body(new SpotMissingTargetReq(spotRid))
            .Async<SpotMissingTargetRes>()).Body;
        ZlinkStreamAssert.Ensure(closedSpotRequest.Failed, "SM-E3 closed spot request did not fail.");
        var expectedEvidence = new[]
        {
            $"timer-idle-close|rid=play-a|spot={spotRid}|name=sm-e3-idle|closed=True",
            $"spot-closing|rid=play-a|spot={spotRid}"
        };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-E3 evidence mismatch.");
        Console.WriteLine("operation SpotService.sm-e3 passed");
    }
}
