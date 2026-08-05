// Verifies SM-C4 Spot Publisher Client behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmC4SpotPublisherClientScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, ZLinkHttpClient gateway)
    {
        var spotRid = $"spot-sm-c4-{Guid.NewGuid():N}";
        var created = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
            "SM-C4 publish spot was not created on play-a.");
        var unsubscribedSpotRid = $"spot-sm-c4-unsubscribed-{Guid.NewGuid():N}";
        var unsubscribedSpot = (await playA.Post("/spot/create-alternate")
            .Body(new CreateSpotReq(unsubscribedSpotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(unsubscribedSpot.SpotRid == unsubscribedSpotRid && unsubscribedSpot.NodeRid == "play-a",
            "SM-C4 unsubscribed spot was not created on play-a.");
        var marker = "sm-c4-publish";
        var waitTask = playA.Post("/spot/publish/wait")
            .Body(new SpotPublishReq(spotRid, marker))
            .Async<SpotPublishObserveRes>();
        var publish = (await gateway.Post("/spot/publish")
            .Body(new SpotPublishReq(spotRid, marker))
            .Async<SpotPublishRes>()).Body;
        var observe = (await waitTask).Body;
        ZlinkStreamAssert.Ensure(publish.Operation == "spot.sm-c4-publish", "SM-C4 publish operation mismatch.");
        ZlinkStreamAssert.Ensure(publish.PublisherRid == "gateway", "SM-C4 publisher was not the publish-only gateway.");
        ZlinkStreamAssert.Ensure(publish.SpotRid == spotRid, "SM-C4 publish target spot mismatch.");
        ZlinkStreamAssert.Ensure(observe.Operation == "spot.sm-c4-observe", "SM-C4 observe operation mismatch.");
        ZlinkStreamAssert.Ensure(observe.Received, "SM-C4 publish-only gateway event was not received.");
        ZlinkStreamAssert.Ensure(
            publish.Evidence.Any(line =>
                line.Contains($"spot-publish|rid=gateway|spot={spotRid}|marker={marker}", StringComparison.Ordinal)),
            "SM-C4 gateway evidence did not include publish marker.");
        ZlinkStreamAssert.Ensure(
            observe.Evidence.Any(line =>
                line.Contains($"spot-msg|rid=play-a|spot={spotRid}|marker={marker}", StringComparison.Ordinal)),
            "SM-C4 evidence did not include spot event publish.");
        ZlinkStreamAssert.Ensure(
            observe.Evidence.All(line =>
                !line.Contains($"spot-msg|rid=play-a|spot={unsubscribedSpotRid}|marker={marker}",
                    StringComparison.Ordinal)),
            "SM-C4 unsubscribed spot received publish event.");
        Console.WriteLine("operation SpotService.sm-c4 passed");
    }
}
