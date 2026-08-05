// Provides the shared multi-node routing probe used by SpotService routing scenarios.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

// Verifies multi-node route-to-spot behavior.
internal static class MultiNodeSpotRoutingProbe
{
    public static async Task RunAsync(ZLinkHttpClient multiA, ZLinkHttpClient multiB)
    {
        var spotA = $"spot-sm-q9-a-{Guid.NewGuid():N}";
        var spotB = $"spot-sm-q9-b-{Guid.NewGuid():N}";
        var createdA = (await multiA.Post("/spot/create-local")
            .Body(new MultiNodeCreateSpotReq(spotA, 0))
            .Async<MultiNodeCreateSpotRes>()).Body;
        var firstA = (await multiA.Post("/spot/state/request")
            .Body(new MultiNodeStateRouteReq(spotA, 11))
            .Async<StateRes>()).Body;
        var directA = (await multiA.Post("/spot/state/request")
            .Body(new MultiNodeStateRouteReq(spotA, 0))
            .Async<StateRes>()).Body;
        var evidenceA = (await multiA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"multi-state-request|node={SpotServiceNames.MultiSpotNodeA}|spot={spotA}|value=11"
            ]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidenceA.Count(line =>
                line == $"multi-state-request|node={SpotServiceNames.MultiSpotNodeA}|spot={spotA}|value=11") >= 2,
            "SM-Q9 node A did not process both route-to-spot requests.");

        var createdB = (await multiB.Post("/spot/create-local")
            .Body(new MultiNodeCreateSpotReq(spotB, 0))
            .Async<MultiNodeCreateSpotRes>()).Body;
        var firstB = (await multiB.Post("/spot/state/request")
            .Body(new MultiNodeStateRouteReq(spotB, 17))
            .Async<StateRes>()).Body;
        var directB = (await multiB.Post("/spot/state/request")
            .Body(new MultiNodeStateRouteReq(spotB, 0))
            .Async<StateRes>()).Body;
        var evidenceB = (await multiB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                $"multi-state-request|node={SpotServiceNames.MultiSpotNodeB}|spot={spotB}|value=17"
            ]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidenceB.Count(line =>
                line == $"multi-state-request|node={SpotServiceNames.MultiSpotNodeB}|spot={spotB}|value=17") >= 2,
            "SM-Q9 node B did not process both route-to-spot requests.");

        ZlinkStreamAssert.Ensure(createdA.NodeRid == SpotServiceNames.MultiSpotNodeA,
            "SM-Q9 node A create reply node mismatch.");
        ZlinkStreamAssert.Ensure(firstA.Value == 11, "SM-Q9 node A route-to-spot reply value mismatch.");
        ZlinkStreamAssert.Ensure(directA.SpotRid == spotA, "SM-Q9 node A direct spot reply target mismatch.");
        ZlinkStreamAssert.Ensure(directA.NodeRid == SpotServiceNames.MultiSpotNodeA,
            "SM-Q9 node A direct spot reply node mismatch.");
        ZlinkStreamAssert.Ensure(directA.Value == 11, "SM-Q9 node A direct spot reply value mismatch.");
        ZlinkStreamAssert.Ensure(createdB.NodeRid == SpotServiceNames.MultiSpotNodeB,
            "SM-Q9 node B create reply node mismatch.");
        ZlinkStreamAssert.Ensure(firstB.Value == 17, "SM-Q9 node B route-to-spot reply value mismatch.");
        ZlinkStreamAssert.Ensure(directB.SpotRid == spotB, "SM-Q9 node B direct spot reply target mismatch.");
        ZlinkStreamAssert.Ensure(directB.NodeRid == SpotServiceNames.MultiSpotNodeB,
            "SM-Q9 node B direct spot reply node mismatch.");
        ZlinkStreamAssert.Ensure(directB.Value == 17, "SM-Q9 node B direct spot reply value mismatch.");
        Console.WriteLine("operation SpotService.sm-q9 passed");
    }
}
