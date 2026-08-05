// Verifies SM-A6 Spot Initialize Close Lifecycle behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA6SpotInitializeCloseLifecycleScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var spotRid = $"spot-sm-a6-{Guid.NewGuid():N}";
        var created = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
            "SM-A6 lifecycle spot was not created on play-a.");
        var closeReply = (await playA.Post("/spot/close")
            .Body(new CloseSpotReq(spotRid))
            .Async<CloseSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(closeReply.Closed, "SM-A6 did not close the lifecycle spot.");
        var expectedEvidence = new[]
        {
            $"spot-initialize|rid=play-a|spot={spotRid}",
            $"spot-closing|rid=play-a|spot={spotRid}"
        };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-A6 evidence mismatch.");
        Console.WriteLine("operation SpotService.sm-a6 passed");
    }
}
