// Verifies SM-A12 automatic Spot IDs under concurrent creation.
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA12AutomaticSpotIdsScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        const int count = 200;
        var result = (await playA.Post("/spot/create-automatic-batch")
                .Body(new AutomaticSpotBatchReq(count))
                .Async<AutomaticSpotBatchRes>()).Body;

        ZlinkStreamAssert.Ensure(
            result.Requested == count
                && result.Created == count
                && result.DistinctIds == count
                && result.SuccessfulRequests == count
                && result.SpotIds.Length == count,
            "SM-A12 automatic Spot creation did not return distinct independent objects.");
        ZlinkStreamAssert.Ensure(
            result.SpotIds.Distinct(StringComparer.Ordinal).Count() == count,
            "SM-A12 returned duplicate automatic Spot IDs.");
        Console.WriteLine("operation SpotService.sm-a12 passed");
    }
}
