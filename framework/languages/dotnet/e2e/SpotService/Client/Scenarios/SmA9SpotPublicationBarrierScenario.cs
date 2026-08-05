// Verifies SM-A9 Spot publication waits for completed initial membership.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA9SpotPublicationBarrierScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, ZLinkHttpClient playB)
    {
        var spotId = $"spot-sm-a9-{Guid.NewGuid():N}";
        try
        {
            await SetWeightsAsync(playA, playB, 100, 0);
            await playA.Post("/spot/a9/start")
                .Body(new GatedSpotCreateReq(spotId))
                .Async<GatedSpotCreateRes>();
            await playA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq([
                    $"spot-initialize-started|rid=play-a|spot={spotId}"
                ]))
                .Async<string[]>();

            var beforeRelease = await ProbeAsync(playB, spotId);
            ZlinkStreamAssert.Ensure(
                !beforeRelease.Found && !beforeRelease.RequestSucceeded,
                "SM-A9 exposed or served a Spot before Initialize completed.");

            Console.WriteLine("spot-service sm-a9 release-play-a-ready");
            var created = (await playA.Post("/spot/a9/status")
                .Body(new GatedSpotCreateReq(spotId))
                .Async<GatedSpotCreateRes>()).Body;
            ZlinkStreamAssert.Ensure(
                (created.State is "Created" or "Existing")
                && created.NodeRid.StartsWith("play-a-", StringComparison.Ordinal),
                $"SM-A9 gated Spot did not complete on play-a: state={created.State}, node={created.NodeRid}.");

            var afterRelease = await ProbeAsync(playB, spotId);
            ZlinkStreamAssert.Ensure(
                afterRelease.Found
                && afterRelease.RequestSucceeded
                && afterRelease.FoundNodeRid == created.NodeRid
                && afterRelease.RequestNodeRid == created.NodeRid
                && afterRelease.Value == 1,
                "SM-A9 Find/request did not converge on the published Spot.");
            Console.WriteLine("operation SpotService.sm-a9 passed");
        }
        finally
        {
            await SetWeightsAsync(playA, playB, 100, 100);
        }
    }

    private static async Task<SpotPublicationProbeRes> ProbeAsync(
        ZLinkHttpClient play,
        string spotId) =>
        (await play.Post("/spot/a9/probe")
            .Body(new GatedSpotCreateReq(spotId))
            .Async<SpotPublicationProbeRes>()).Body;

    private static async Task SetWeightsAsync(
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
