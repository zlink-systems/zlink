// Verifies SM-E2 Spot Timer Tick behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmE2SpotTimerTickScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var spotRid = $"spot-sm-e2-{Guid.NewGuid():N}";
        var created = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
            "SM-E2 timer spot was not created on play-a.");
        var timer = (await playA.Post("/spot/timer/start")
            .Body(new SpotTimerStartReq(spotRid, "sm-e2-basic", 50))
            .Async<SpotTimerStartRes>()).Body;
        ZlinkStreamAssert.Ensure(timer.Started, "SM-E2 timer did not start.");
        var closeReply = (await playA.Post("/spot/close")
            .Body(new CloseSpotReq(spotRid))
            .Async<CloseSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(closeReply.Closed, "SM-E2 timer spot close reply mismatch.");
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"timer-basic|rid=play-a|spot={spotRid}|name=sm-e2-basic"]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Count(line => line.Contains(
                $"timer-basic|rid=play-a|spot={spotRid}|name=sm-e2-basic",
                StringComparison.Ordinal)) >= 2,
            "SM-E2 evidence mismatch.");
        Console.WriteLine("operation SpotService.sm-e2 passed");
    }
}
