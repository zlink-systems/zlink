// Verifies SM-A10 Entry Spot identity is stable and externally observable.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA10EntrySpotIdentityScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var first = await IdentityAsync(playA);
        var stable = await IdentityAsync(playA);
        ZlinkStreamAssert.Ensure(
            first == stable
            && first.NodeRid.StartsWith("play-a-", StringComparison.Ordinal)
            && first.EntrySpotId.StartsWith("play-", StringComparison.Ordinal)
            && first.EntrySpotId.Contains("-entry-", StringComparison.Ordinal)
            && first.EntrySpotId != first.NodeRid,
            "SM-A10 Entry Spot identity was not stable or valid within one lifecycle.");

        var firstSpot = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq($"spot-sm-a10-before-{Guid.NewGuid():N}"))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(
            firstSpot.NodeRid == "play-a",
            "SM-A10 could not use the Entry Object Server before restart.");

        Console.WriteLine("spot-service sm-a10 restart-play-a-ready");
        EntryIdentityRes? replacement = null;
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(20);
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                var candidate = await IdentityAsync(playA);
                if (candidate.EntrySpotId != first.EntrySpotId)
                {
                    replacement = candidate;
                    break;
                }
            }
            catch
            {
                // The replacement process is between its old and new listener.
            }

            await Task.Delay(100);
        }

        ZlinkStreamAssert.Ensure(
            replacement is not null
            && replacement.NodeRid.StartsWith("play-a-", StringComparison.Ordinal)
            && replacement.EntrySpotId.StartsWith("play-", StringComparison.Ordinal)
            && replacement.EntrySpotId.Contains("-entry-", StringComparison.Ordinal)
            && replacement.EntrySpotId != first.EntrySpotId,
            $"SM-A10 replacement lifecycle kept the old Entry Spot identity: old={first.EntrySpotId}, new={replacement?.EntrySpotId ?? "<none>"}.");

        var replacementSpot = (await playA.Post("/spot/create")
            .Body(new CreateSpotReq($"spot-sm-a10-after-{Guid.NewGuid():N}"))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(
            replacementSpot.NodeRid == "play-a",
            "SM-A10 replacement lifecycle could not serve a new Entry request.");
        Console.WriteLine("operation SpotService.sm-a10 passed");
    }

    private static async Task<EntryIdentityRes> IdentityAsync(ZLinkHttpClient playA) =>
        (await playA.Get("/entry/identity").Async<EntryIdentityRes>()).Body;
}
