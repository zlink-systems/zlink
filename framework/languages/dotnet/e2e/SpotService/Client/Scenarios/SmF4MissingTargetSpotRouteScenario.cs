// Verifies SM-F4 Missing Target Spot Route behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmF4MissingTargetSpotRouteScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var missingTarget = (await playA.Post("/spot/missing-target/request")
            .Body(new SpotMissingTargetReq($"missing-spot-sm-f4-{Guid.NewGuid():N}"))
            .Async<SpotMissingTargetRes>()).Body;
        ZlinkStreamAssert.Ensure(missingTarget.Failed, "SM-F4 missing target request did not fail.");
        Console.WriteLine("operation SpotService.sm-f4 passed");
    }
}
