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
        await playA.Post("/spot/create")
            .Body(new CreateSpotReq(sourceSpotRid))
            .Async<CreateSpotRes>();
        await playB.Post("/spot/create")
            .Body(new CreateSpotReq(targetSpotRid))
            .Async<CreateSpotRes>();

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
}
