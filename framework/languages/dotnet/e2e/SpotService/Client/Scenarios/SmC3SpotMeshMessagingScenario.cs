// Verifies SM-C3 Spot Mesh Messaging behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

// Verifies direct spot-to-spot request, send, publish, timeout, and missing handler behavior.
internal static class SmC3SpotMeshMessagingScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, ZLinkHttpClient playB)
    {
        var sourceSpotRid = $"spot-sm-c3-source-{Guid.NewGuid():N}";
        var targetSpotRid = $"spot-sm-c3-target-{Guid.NewGuid():N}";
        await SetPlacementWeightsAsync(playA, playB, 100, 0);
        try
        {
            var target = (await playA.Post("/spot/create")
                .Body(new CreateSpotReq(targetSpotRid))
                .Async<CreateSpotRes>()).Body;
            ZlinkStreamAssert.Ensure(target.SpotRid == targetSpotRid && target.NodeRid == "play-a",
                "SM-C3 target spot was not created on play-a.");

            await SetPlacementWeightsAsync(playA, playB, 0, 100);
            var source = (await playB.Post("/spot/create")
                .Body(new CreateSpotReq(sourceSpotRid))
                .Async<CreateSpotRes>()).Body;
            ZlinkStreamAssert.Ensure(source.SpotRid == sourceSpotRid && source.NodeRid == "play-b",
                "SM-C3 source spot was not created on play-b.");

            var direct = (await playB.Post("/spot/to-spot/request-cross")
                .Body(new SpotToSpotRouteReq(sourceSpotRid, targetSpotRid, "direct"))
                .Async<SpotToSpotRes>()).Body;
            ZlinkStreamAssert.Ensure(direct.SourceSpotRid == sourceSpotRid, "SM-C3 source spot mismatch.");
            ZlinkStreamAssert.Ensure(direct.TargetSpotRid == targetSpotRid, "SM-C3 target spot mismatch.");
            ZlinkStreamAssert.Ensure(direct.TargetValue >= 3, "SM-C3 target state was not updated.");

            var timeout = (await playB.Post("/spot/to-spot/timeout")
                .Body(new SpotToSpotTimeoutRouteReq(sourceSpotRid, targetSpotRid, "slow"))
                .Async<SpotToSpotTimeoutRes>()).Body;
            ZlinkStreamAssert.Ensure(timeout.Failed, "SM-C3 slow target request did not time out.");

            var negative = (await playB.Post("/spot/to-spot/negative-cross")
                .Body(new SpotToSpotNegativeRouteReq(sourceSpotRid, targetSpotRid, "missing"))
                .Async<SpotToSpotNegativeRes>()).Body;
            ZlinkStreamAssert.Ensure(negative.RequestFailed, "SM-C3 missing target handler request did not fail.");

            var expectedPlayA = new[]
            {
                $"spot-state-command|rid=play-a|spot={targetSpotRid}|marker=sm-c3-send-direct",
                $"spot-msg|rid=play-a|spot={targetSpotRid}|marker=sm-c3-publish-direct",
                "dispatch-error|surface=SpotRoute|reason=HandlerMissing|action=ReplyError|packet=MissingSpotReq",
                "dispatch-error|surface=SpotRoute|reason=HandlerMissing|action=Drop|packet=MissingSpotMsg"
            };
            var expectedPlayB = new[]
            {
                $"spot-to-spot|rid=play-b|source={sourceSpotRid}|target={targetSpotRid}|value=",
                $"spot-to-spot-timeout|rid=play-b|source={sourceSpotRid}|target={targetSpotRid}|failed=True",
                $"spot-to-spot-negative|rid=play-b|source={sourceSpotRid}|target={targetSpotRid}|requestFailed=True"
            };
            await WaitForEvidenceAsync(playA, expectedPlayA);
            await WaitForEvidenceAsync(playB, expectedPlayB);

            Console.WriteLine("operation SpotService.sm-c3 passed");
        }
        finally
        {
            await SetPlacementWeightsAsync(playA, playB, 100, 100);
        }
    }

    private static async Task SetPlacementWeightsAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        int playAWeight,
        int playBWeight)
    {
        await playA.Post("/placement-weight")
            .Body(new PlacementWeightReq(playAWeight))
            .Async<PlacementWeightRes>();
        await playB.Post("/placement-weight")
            .Body(new PlacementWeightReq(playBWeight))
            .Async<PlacementWeightRes>();
        await Task.Delay(TimeSpan.FromSeconds(2));
    }

    private static async Task WaitForEvidenceAsync(
        ZLinkHttpClient client,
        IReadOnlyList<string> expected)
    {
        var evidence = (await client.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expected.ToArray()))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expected.All(item => evidence.Any(line =>
                line.Contains(item, StringComparison.Ordinal))),
            "SM-C3 evidence mismatch.");
    }
}
