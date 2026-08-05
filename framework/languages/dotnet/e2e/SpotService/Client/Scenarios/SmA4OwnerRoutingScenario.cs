// Verifies SM-A4 Owner Routing behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA4OwnerRoutingScenario
{
    public static async Task RunAsync(ZLinkHttpClient api)
    {
        var spotRid = $"spot-sm-a4-{Guid.NewGuid():N}";
        await api.Post("/spot/create").Body(new CreateSpotReq(spotRid)).Async<CreateSpotRes>();
        var reply = (await api.Post("/spot/state/request")
            .Body(new SpotStateRouteReq(spotRid, "noop", 0))
            .Async<StateRes>()).Body;
        ZlinkStreamAssert.Ensure(reply.SpotRid == spotRid, "SM-A4 request reached the wrong spot.");
        ZlinkStreamAssert.Ensure(reply.NodeRid == "play-a", "SM-A4 owner routing did not stay on play-a.");
        Console.WriteLine("operation SpotService.sm-a4 passed");
    }
}
