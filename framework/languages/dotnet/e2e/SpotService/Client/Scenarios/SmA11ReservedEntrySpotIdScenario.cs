// Verifies reserved Entry Spot IDs fail before factory or Store access.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA11ReservedEntrySpotIdScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        const string reservedSpotId =
            "play-entry-f67e5507-21c6-4a15-bfd1-4a240bfab371";
        var result = (await playA.Post("/spot/reserved-id/probe")
            .Body(new ReservedSpotIdProbeReq(reservedSpotId))
            .Async<ReservedSpotIdProbeRes>()).Body;
        var expected = ZLinkFrameworkErrorKind.InvalidOperation.ToString();
        ZlinkStreamAssert.Ensure(
            result.UserSpotErrorKind == expected
            && result.InstanceSpotErrorKind == expected,
            "SM-A11 reserved Entry Spot ID did not fail both calls with InvalidOperation.");
        ZlinkStreamAssert.Ensure(
            result.UserSpotFactoryCalls == 0
            && result.InstanceSpotFactoryCalls == 0,
            "SM-A11 reserved Entry Spot ID reached a Spot factory.");
        ZlinkStreamAssert.Ensure(
            result.LocationStoreReads == 0
            && result.LocationStoreWrites == 0,
            "SM-A11 reserved Entry Spot ID reached the Location Store.");
        Console.WriteLine("operation SpotService.sm-a11 passed");
    }
}
