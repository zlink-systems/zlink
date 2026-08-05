// Verifies SM-C5 Remote Spot Publish Subscribe behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

// Verifies that a SpotMesh publish from a spot on play-a is observed by a
// subscribed spot on play-b; publisher-side success is not enough.
internal static class SmC5RemoteSpotPublishSubscribeScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, ZLinkHttpClient playB)
    {
        var sourceSpotRid = $"spot-sm-c5-source-{Guid.NewGuid():N}";
        var targetSpotRid = $"spot-sm-c5-target-{Guid.NewGuid():N}";
        await SetPlacementWeightsAsync(playA, playB, 100, 0);
        try
        {
            var source = (await playA.Post("/spot/create")
                .Body(new CreateSpotReq(sourceSpotRid))
                .Async<CreateSpotRes>()).Body;
            ZlinkStreamAssert.Ensure(source.NodeRid == "play-a",
                "SM-C5 source spot was not created on play-a.");

            await SetPlacementWeightsAsync(playA, playB, 0, 100);
            var target = (await playB.Post("/spot/create")
                .Body(new CreateSpotReq(targetSpotRid))
                .Async<CreateSpotRes>()).Body;
            ZlinkStreamAssert.Ensure(target.NodeRid == "play-b",
                "SM-C5 target spot was not created on play-b.");

            var marker = $"sm-c5-{Guid.NewGuid():N}";
            await playA.Post("/spot/to-spot/request-cross")
                .Body(new SpotToSpotRouteReq(sourceSpotRid, targetSpotRid, marker))
                .Async<SpotToSpotRes>();

            var publishMarker = $"sm-c3-publish-{marker}";
            var evidence = (await playB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq([
                    $"spot-msg|rid=play-b|spot={targetSpotRid}|marker={publishMarker}"
                ]))
                .Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                evidence.Any(line =>
                    line.Contains($"spot-msg|rid=play-b|spot={targetSpotRid}|marker={publishMarker}",
                        StringComparison.Ordinal)),
                "SM-C5 cross-node SpotMesh publish did not reach play-b subscriber evidence.");
            Console.WriteLine("operation SpotService.sm-c5 passed");
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
}
