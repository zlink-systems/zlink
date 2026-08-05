// Verifies SM-A1 Entry Spot Request behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA1EntrySpotRequestScenario
{
    public static async Task RunAsync(ZLinkHttpClient api)
    {
        var spotRid = $"spot-sm-a1-{Guid.NewGuid():N}";
        var created = (await api.Post("/spot/create")
            .Body(new CreateSpotReq(spotRid))
            .Async<CreateSpotRes>()).Body;
        ZlinkStreamAssert.Ensure(created.SpotRid == spotRid, "SM-A1 did not create the requested spot.");
        Console.WriteLine("operation SpotService.sm-a1 passed");
    }
}
